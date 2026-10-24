//! Landing: apply a Kindle flight log and produce the next flight plan.
use crate::db::{Db, DbRef};
use anyhow::{bail, Result};
use rusqlite::{params, OptionalExtension};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use sha2::{Digest, Sha256};

pub const PROTO: i64 = 1;
const MAX_ENTRIES: usize = 10_000;

#[derive(Deserialize)]
pub struct Land {
    #[serde(rename = "type")]
    pub kind: String,
    pub proto: i64,
    pub device_id: String,
    pub token: String,
    #[serde(default)]
    pub clock: i64,
    #[serde(default)]
    pub entries: Vec<Value>,
}

#[derive(Deserialize)]
struct Entry {
    id: String,
    seq: i64,
    plan_version: i64,
    #[serde(default)]
    wall: i64,
    kind: String,
    task_id: Option<String>,
    title: Option<String>,
    focus_sec: Option<i64>,
    break_sec: Option<i64>,
    status: Option<String>,
    local_date: Option<String>,
}

#[derive(Serialize)]
pub struct PlanTask {
    pub id: String,
    pub title: String,
    pub position: i64,
    pub review: bool,
}

#[derive(Serialize)]
pub struct Plan {
    pub version: i64,
    pub settings: Value,
    pub tasks: Vec<PlanTask>,
}

pub fn token_hash(token: &str) -> String {
    hex::encode(Sha256::digest(token.as_bytes()))
}

fn ct_eq(a: &str, b: &str) -> bool {
    a.len() == b.len() && a.bytes().zip(b.bytes()).fold(0u8, |x, (p, q)| x | (p ^ q)) == 0
}

fn validate(e: &Entry) -> Result<()> {
    if e.id.is_empty() || e.id.len() > 64 || e.seq < 1 || e.plan_version < 0 {
        bail!("bad entry header");
    }
    let date_ok = |d: &Option<String>| {
        d.as_deref()
            .map(|s| s.len() == 10 && s.chars().all(|c| c.is_ascii_digit() || c == '-'))
            .unwrap_or(false)
    };
    match e.kind.as_str() {
        "session" => {
            let f = e.focus_sec.unwrap_or(-1);
            let st = e.status.as_deref().unwrap_or("");
            if e.task_id.is_none()
                || !(0..=86_400).contains(&f)
                || !["completed", "interrupted", "cancelled"].contains(&st)
                || !date_ok(&e.local_date)
                || e.title.as_deref().map(|t| t.len() > 800).unwrap_or(false)
            {
                bail!("bad session entry");
            }
        }
        "break" => {
            if !(0..=86_400).contains(&e.break_sec.unwrap_or(-1)) || !date_ok(&e.local_date) {
                bail!("bad break entry");
            }
        }
        "done" | "reopen" => {
            if e.task_id.is_none() {
                bail!("missing task_id");
            }
        }
        _ => bail!("unknown entry kind"),
    }
    Ok(())
}

pub fn plan(db: &Db) -> Result<Plan> {
    let tasks = db
        .tasks(true)?
        .into_iter()
        .map(|t| PlanTask {
            id: t.id,
            title: t.title,
            position: t.position,
            review: t.review,
        })
        .collect();
    Ok(Plan {
        version: db.meta_i64("plan_version")?,
        settings: serde_json::json!({
            "focus_min": db.meta_i64("focus_min")?,
            "break_min": db.meta_i64("break_min")?,
        }),
        tasks,
    })
}

pub fn now() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0)
}

/// Handle one request line. Always returns a reply value.
pub fn handle(db: &mut Db, line: &str) -> Value {
    match land(db, line) {
        Ok(v) => v,
        Err(e) => {
            let msg = e.to_string();
            let code = if msg.starts_with("auth") {
                "auth"
            } else if msg.starts_with("proto") {
                "proto"
            } else {
                "bad_entry"
            };
            serde_json::json!({"type":"error","code":code,"message":msg})
        }
    }
}

fn land(db: &mut Db, line: &str) -> Result<Value> {
    let req: Land = serde_json::from_str(line).map_err(|e| anyhow::anyhow!("proto: {e}"))?;
    if req.kind != "land" || req.proto != PROTO {
        bail!("proto: unsupported message or version");
    }
    let stored: Option<String> = db
        .conn
        .query_row(
            "SELECT token_sha256 FROM devices WHERE id=?1",
            [&req.device_id],
            |r| r.get(0),
        )
        .optional()?;
    match stored {
        Some(h) if ct_eq(&h, &token_hash(&req.token)) => {}
        _ => bail!("auth: unknown device or bad token"),
    }
    if req.entries.len() > MAX_ENTRIES {
        bail!("proto: too many entries");
    }
    // Validate everything before writing anything.
    let mut entries = Vec::with_capacity(req.entries.len());
    for raw in &req.entries {
        let e: Entry = serde_json::from_value(raw.clone())
            .map_err(|e| anyhow::anyhow!("bad entry: {e}"))?;
        validate(&e)?;
        entries.push((e, raw.to_string()));
    }

    let dev = req.device_id.clone();
    let tx = db.conn.transaction()?;
    for (e, body) in &entries {
        let inserted = tx.execute(
            "INSERT OR IGNORE INTO entries(device_id,id,seq,kind,task_id,plan_version,wall,local_date,focus_sec,status,title,body)
             VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12)",
            params![
                dev, e.id, e.seq, e.kind, e.task_id, e.plan_version, e.wall, e.local_date,
                e.focus_sec.or(e.break_sec), e.status, e.title, body
            ],
        )?;
        if inserted == 0 {
            continue; // redelivery: no extra effect
        }
        if e.kind == "done" || e.kind == "reopen" {
            let tid = e.task_id.as_deref().unwrap();
            let task: Option<(String, i64)> = tx
                .query_row(
                    "SELECT status,changed_ver FROM tasks WHERE id=?1",
                    [tid],
                    |r| Ok((r.get(0)?, r.get(1)?)),
                )
                .optional()?;
            let want = if e.kind == "done" { "done" } else { "open" };
            match task {
                Some((st, cv)) if cv <= e.plan_version && st != "archived" => {
                    if st != want {
                        DbRef(&tx).bump()?;
                        // Keeps changed_ver: this is a Kindle change, not a computer one.
                        tx.execute(
                            "UPDATE tasks SET status=?1 WHERE id=?2",
                            params![want, tid],
                        )?;
                    }
                }
                Some((st, _)) if st == want => {} // already in the requested state
                Some(_) => {
                    tx.execute(
                        "INSERT INTO conflicts(task_id,device_id,entry_id,kind) VALUES(?1,?2,?3,?4)",
                        params![tid, dev, e.id, e.kind],
                    )?;
                    DbRef(&tx).bump()?;
                }
                None => {} // unknown task: record kept in entries, nothing to apply
            }
        }
    }
    let acked: i64 = tx.query_row(
        "SELECT COALESCE(MAX(seq),0) FROM entries WHERE device_id=?1",
        [&dev],
        |r| r.get(0),
    )?;
    let t = now();
    let offset = if req.clock > 0 { Some(req.clock - t) } else { None };
    tx.execute(
        "UPDATE devices SET acked_seq=?1, last_landing=?2, clock_offset=?3 WHERE id=?4",
        params![acked, t, offset, dev],
    )?;
    tx.commit()?;

    let p = plan(db)?;
    db.conn.execute(
        "UPDATE devices SET plan_version=?1 WHERE id=?2",
        params![p.version, dev],
    )?;
    Ok(serde_json::json!({"type":"landed","acked_seq":acked,"plan":p}))
}

pub fn add_device(db: &Db, token: &str) -> Result<String> {
    let id = uuid::Uuid::new_v4().to_string();
    db.conn.execute(
        "INSERT INTO devices(id,token_sha256) VALUES(?1,?2)",
        params![id, token_hash(token)],
    )?;
    Ok(id)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn setup() -> (Db, String, String, tempfile::TempDir) {
        let d = tempfile::tempdir().unwrap();
        let db = Db::open(&d.path().join("t.db")).unwrap();
        let dev = add_device(&db, "tok").unwrap();
        (db, dev, "tok".into(), d)
    }

    fn land_req(dev: &str, tok: &str, entries: Vec<Value>) -> String {
        json!({"type":"land","proto":1,"device_id":dev,"token":tok,"clock":1,"entries":entries}).to_string()
    }

    fn session(id: &str, seq: i64, task: &str, pv: i64) -> Value {
        json!({"id":id,"seq":seq,"plan_version":pv,"wall":0,"kind":"session","task_id":task,
               "title":"T","planned_min":25,"focus_sec":1500,"status":"completed","local_date":"2026-10-24"})
    }

    fn req(id: &str, seq: i64, kind: &str, task: &str, pv: i64) -> Value {
        json!({"id":id,"seq":seq,"plan_version":pv,"wall":0,"kind":kind,"task_id":task})
    }

    fn status_of(db: &Db, id: &str) -> String {
        db.conn.query_row("SELECT status FROM tasks WHERE id=?1", [id], |r| r.get(0)).unwrap()
    }

    #[test]
    fn rejects_bad_token() {
        let (mut db, dev, _, _d) = setup();
        let r = handle(&mut db, &land_req(&dev, "wrong", vec![]));
        assert_eq!(r["code"], "auth");
    }

    #[test]
    fn sessions_accepted_and_redelivery_idempotent() {
        let (mut db, dev, tok, _d) = setup();
        let t = db.add("A").unwrap();
        let body = land_req(&dev, &tok, vec![session("e1", 1, &t, 1), session("e2", 2, &t, 1)]);
        let r1 = handle(&mut db, &body);
        assert_eq!(r1["acked_seq"], 2);
        let r2 = handle(&mut db, &body);
        assert_eq!(r2["acked_seq"], 2);
        let n: i64 = db.conn.query_row("SELECT COUNT(*) FROM entries", [], |r| r.get(0)).unwrap();
        assert_eq!(n, 2);
        assert_eq!(r1["plan"]["tasks"][0]["id"], t);
    }

    #[test]
    fn done_applies_when_computer_untouched() {
        let (mut db, dev, tok, _d) = setup();
        let t = db.add("A").unwrap();
        let pv = db.meta_i64("plan_version").unwrap();
        db.add("B").unwrap(); // unrelated change after departure
        let r = handle(&mut db, &land_req(&dev, &tok, vec![req("e1", 1, "done", &t, pv)]));
        assert_eq!(status_of(&db, &t), "done");
        assert_eq!(r["plan"]["tasks"].as_array().unwrap().len(), 1, "done task not carried");
        assert!(db.conflicts().unwrap().is_empty());
    }

    #[test]
    fn done_conflicts_when_computer_edited() {
        let (mut db, dev, tok, _d) = setup();
        let t = db.add("A").unwrap();
        let pv = db.meta_i64("plan_version").unwrap();
        db.edit(&t, "A renamed").unwrap();
        let r = handle(&mut db, &land_req(&dev, &tok, vec![req("e1", 1, "done", &t, pv)]));
        assert_eq!(status_of(&db, &t), "open", "computer state stands");
        assert_eq!(r["plan"]["tasks"][0]["review"], true);
        assert_eq!(db.resolve(&t, true).unwrap(), 1);
        assert_eq!(status_of(&db, &t), "done");
    }

    #[test]
    fn archived_task_request_conflicts() {
        let (mut db, dev, tok, _d) = setup();
        let t = db.add("A").unwrap();
        let pv = db.meta_i64("plan_version").unwrap();
        db.set_status(&t, "archived").unwrap();
        handle(&mut db, &land_req(&dev, &tok, vec![req("e1", 1, "done", &t, pv)]));
        assert_eq!(status_of(&db, &t), "archived");
        assert_eq!(db.conflicts().unwrap().len(), 1);
        db.resolve(&t, true).unwrap();
        assert_eq!(status_of(&db, &t), "archived", "archive is not undone by Kindle");
    }

    #[test]
    fn move_does_not_invalidate_requests() {
        let (mut db, dev, tok, _d) = setup();
        let a = db.add("A").unwrap();
        db.add("B").unwrap();
        let pv = db.meta_i64("plan_version").unwrap();
        db.move_to(&a, 2).unwrap();
        handle(&mut db, &land_req(&dev, &tok, vec![req("e1", 1, "done", &a, pv)]));
        assert_eq!(status_of(&db, &a), "done");
    }

    #[test]
    fn invalid_entry_rejects_whole_landing() {
        let (mut db, dev, tok, _d) = setup();
        let t = db.add("A").unwrap();
        let bad = json!({"id":"x","seq":2,"plan_version":1,"kind":"rm -rf","task_id":t});
        let r = handle(&mut db, &land_req(&dev, &tok, vec![session("e1", 1, &t, 1), bad]));
        assert_eq!(r["type"], "error");
        let n: i64 = db.conn.query_row("SELECT COUNT(*) FROM entries", [], |r| r.get(0)).unwrap();
        assert_eq!(n, 0, "nothing written");
    }
}
