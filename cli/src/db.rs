use anyhow::{anyhow, bail, Result};
use rusqlite::{params, Connection, OptionalExtension};
use std::path::Path;

pub struct Db {
    pub conn: Connection,
}

#[derive(Debug, Clone)]
pub struct Task {
    pub id: String,
    pub title: String,
    pub position: i64,
    pub status: String,
    pub review: bool,
}

const SCHEMA: &str = "
CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, val TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS tasks (
  id TEXT PRIMARY KEY,
  title TEXT NOT NULL,
  position INTEGER NOT NULL,
  status TEXT NOT NULL DEFAULT 'open',
  changed_ver INTEGER NOT NULL,
  created INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS devices (
  id TEXT PRIMARY KEY,
  token_sha256 TEXT NOT NULL,
  acked_seq INTEGER NOT NULL DEFAULT 0,
  plan_version INTEGER NOT NULL DEFAULT 0,
  last_landing INTEGER,
  clock_offset INTEGER
);
CREATE TABLE IF NOT EXISTS entries (
  device_id TEXT NOT NULL,
  id TEXT NOT NULL,
  seq INTEGER NOT NULL,
  kind TEXT NOT NULL,
  task_id TEXT,
  plan_version INTEGER NOT NULL,
  wall INTEGER NOT NULL,
  local_date TEXT,
  focus_sec INTEGER,
  status TEXT,
  title TEXT,
  body TEXT NOT NULL,
  PRIMARY KEY (device_id, id)
);
CREATE TABLE IF NOT EXISTS conflicts (
  rowid INTEGER PRIMARY KEY AUTOINCREMENT,
  task_id TEXT NOT NULL,
  device_id TEXT NOT NULL,
  entry_id TEXT NOT NULL,
  kind TEXT NOT NULL,
  resolved INTEGER NOT NULL DEFAULT 0
);
";

impl Db {
    pub fn open(path: &Path) -> Result<Db> {
        let conn = Connection::open(path)?;
        conn.pragma_update(None, "journal_mode", "WAL")?;
        conn.pragma_update(None, "synchronous", "FULL")?;
        conn.execute_batch(SCHEMA)?;
        let db = Db { conn };
        for (k, v) in [("plan_version", "0"), ("focus_min", "25"), ("break_min", "5")] {
            db.conn.execute(
                "INSERT OR IGNORE INTO meta(key,val) VALUES(?1,?2)",
                params![k, v],
            )?;
        }
        Ok(db)
    }

    pub fn meta_i64(&self, key: &str) -> Result<i64> {
        DbRef(&self.conn).meta_i64(key)
    }

    pub fn resolve_id(&self, prefix: &str) -> Result<String> {
        let mut st = self
            .conn
            .prepare("SELECT id FROM tasks WHERE id LIKE ?1 || '%'")?;
        let ids: Vec<String> = st
            .query_map([prefix], |r| r.get(0))?
            .collect::<Result<_, _>>()?;
        match ids.len() {
            0 => bail!("no task matches '{prefix}'"),
            1 => Ok(ids[0].clone()),
            _ => bail!("'{prefix}' is ambiguous"),
        }
    }

    pub fn add(&mut self, title: &str) -> Result<String> {
        let title = clean_title(title)?;
        let tx = self.conn.transaction()?;
        let db = DbRef(&tx);
        let v = db.bump()?;
        let pos: i64 = tx.query_row(
            "SELECT COALESCE(MAX(position),0)+1 FROM tasks WHERE status!='archived'",
            [],
            |r| r.get(0),
        )?;
        let id = uuid::Uuid::new_v4().to_string();
        tx.execute(
            "INSERT INTO tasks(id,title,position,status,changed_ver,created) VALUES(?1,?2,?3,'open',?4,strftime('%s','now'))",
            params![id, title, pos, v],
        )?;
        tx.commit()?;
        Ok(id)
    }

    /// Computer-side change to a task: records changed_ver so stale Kindle requests conflict.
    fn touch(&mut self, id: &str, sql: &str, arg: &dyn rusqlite::ToSql) -> Result<()> {
        let tx = self.conn.transaction()?;
        let v = DbRef(&tx).bump()?;
        let n = tx.execute(sql, params![arg, id])?;
        if n == 0 {
            bail!("task not found");
        }
        tx.execute("UPDATE tasks SET changed_ver=?1 WHERE id=?2", params![v, id])?;
        tx.commit()?;
        Ok(())
    }

    pub fn edit(&mut self, id: &str, title: &str) -> Result<()> {
        let t = clean_title(title)?;
        self.touch(id, "UPDATE tasks SET title=?1 WHERE id=?2", &t)
    }

    pub fn set_status(&mut self, id: &str, status: &str) -> Result<()> {
        self.touch(id, "UPDATE tasks SET status=?1 WHERE id=?2", &status)
    }

    pub fn move_to(&mut self, id: &str, pos: i64) -> Result<()> {
        let mut ids: Vec<String> = self
            .tasks(false)?
            .into_iter()
            .map(|t| t.id)
            .filter(|t| t != id)
            .collect();
        let idx = (pos.max(1) as usize - 1).min(ids.len());
        ids.insert(idx, id.to_string());
        let tx = self.conn.transaction()?;
        // Ordering bumps the plan but is not a task change, so Kindle requests stay valid.
        DbRef(&tx).bump()?;
        for (i, t) in ids.iter().enumerate() {
            tx.execute(
                "UPDATE tasks SET position=?1 WHERE id=?2",
                params![i as i64 + 1, t],
            )?;
        }
        tx.commit()?;
        Ok(())
    }

    pub fn set_config(&mut self, focus: Option<i64>, brk: Option<i64>) -> Result<()> {
        let tx = self.conn.transaction()?;
        let db = DbRef(&tx);
        if let Some(f) = focus {
            if !(1..=240).contains(&f) {
                bail!("focus must be 1..240 minutes");
            }
            db.set_meta("focus_min", f)?;
        }
        if let Some(b) = brk {
            if !(0..=120).contains(&b) {
                bail!("break must be 0..120 minutes");
            }
            db.set_meta("break_min", b)?;
        }
        db.bump()?;
        tx.commit()?;
        Ok(())
    }

    /// All non-archived tasks (open and done), or only open ones.
    pub fn tasks(&self, open_only: bool) -> Result<Vec<Task>> {
        let sql = format!(
            "SELECT id,title,position,status,
               EXISTS(SELECT 1 FROM conflicts c WHERE c.task_id=tasks.id AND c.resolved=0)
             FROM tasks WHERE status {} ORDER BY status='done', position",
            if open_only { "='open'" } else { "!='archived'" }
        );
        let mut st = self.conn.prepare(&sql)?;
        let rows = st
            .query_map([], |r| {
                Ok(Task {
                    id: r.get(0)?,
                    title: r.get(1)?,
                    position: r.get(2)?,
                    status: r.get(3)?,
                    review: r.get(4)?,
                })
            })?
            .collect::<Result<_, _>>()?;
        Ok(rows)
    }

    pub fn conflicts(&self) -> Result<Vec<(i64, String, String, String)>> {
        let mut st = self.conn.prepare(
            "SELECT c.rowid,c.task_id,COALESCE(t.title,'?'),c.kind FROM conflicts c
             LEFT JOIN tasks t ON t.id=c.task_id WHERE c.resolved=0 ORDER BY c.rowid",
        )?;
        let r = st
            .query_map([], |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?)))?
            .collect::<Result<_, _>>()?;
        Ok(r)
    }

    /// Settle conflicts for a task: accept the Kindle's last request or keep the computer state.
    pub fn resolve(&mut self, id: &str, accept_kindle: bool) -> Result<usize> {
        let last: Option<String> = self
            .conn
            .query_row(
                "SELECT kind FROM conflicts WHERE task_id=?1 AND resolved=0 ORDER BY rowid DESC LIMIT 1",
                [id],
                |r| r.get(0),
            )
            .optional()?;
        let Some(kind) = last else {
            bail!("no open conflict for this task")
        };
        let tx = self.conn.transaction()?;
        let v = DbRef(&tx).bump()?;
        if accept_kindle {
            let st = if kind == "done" { "done" } else { "open" };
            tx.execute(
                "UPDATE tasks SET status=?1, changed_ver=?2 WHERE id=?3 AND status!='archived'",
                params![st, v, id],
            )?;
        }
        let n = tx.execute(
            "UPDATE conflicts SET resolved=1 WHERE task_id=?1 AND resolved=0",
            [id],
        )?;
        tx.commit()?;
        Ok(n)
    }
}

/// Meta helpers usable on a connection or inside a transaction.
pub struct DbRef<'a>(pub &'a Connection);
impl DbRef<'_> {
    pub fn meta_i64(&self, key: &str) -> Result<i64> {
        let v: String = self
            .0
            .query_row("SELECT val FROM meta WHERE key=?1", [key], |r| r.get(0))?;
        Ok(v.parse()?)
    }
    pub fn set_meta(&self, key: &str, val: i64) -> Result<()> {
        self.0.execute(
            "INSERT INTO meta(key,val) VALUES(?1,?2) ON CONFLICT(key) DO UPDATE SET val=?2",
            params![key, val.to_string()],
        )?;
        Ok(())
    }
    /// Bump plan version and return the new one.
    pub fn bump(&self) -> Result<i64> {
        let v = self.meta_i64("plan_version")? + 1;
        self.set_meta("plan_version", v)?;
        Ok(v)
    }
}

pub fn clean_title(t: &str) -> Result<String> {
    let t = t.trim();
    if t.is_empty() {
        return Err(anyhow!("title is empty"));
    }
    if t.chars().count() > 200 {
        bail!("title longer than 200 characters");
    }
    Ok(t.chars().filter(|c| !c.is_control()).collect())
}
