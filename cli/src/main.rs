mod db;
mod net;
mod sync;

use anyhow::{Context, Result};
use clap::{Parser, Subcommand};
use db::Db;
use std::path::PathBuf;

#[derive(Parser)]
#[command(name = "pomo-kndl", version, about = "Focus timer hub: plans tasks, lands Kindle flight logs")]
struct Cli {
    /// Data directory (default: per-user app data, or $POMO_KNDL_HOME)
    #[arg(long, global = true)]
    home: Option<PathBuf>,
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Add a task
    Add { title: String },
    /// Rename a task
    Edit { id: String, title: String },
    /// Move a task to a position (1 = top)
    Move { id: String, position: i64 },
    /// List tasks
    Tasks {
        #[arg(long)]
        all: bool,
    },
    /// Mark a task done
    Done { id: String },
    /// Reopen a task
    Reopen { id: String },
    /// Archive a task (no longer carried to the Kindle)
    Archive { id: String },
    /// Show or set durations, e.g. `config --focus 25 --break 5`
    Config {
        #[arg(long)]
        focus: Option<i64>,
        #[arg(long = "break")]
        brk: Option<i64>,
    },
    /// Session history from landed flight logs
    History,
    /// Landing and review status
    Status,
    /// Settle a review: --kindle accepts the Kindle request, --keep keeps computer state
    Resolve {
        id: String,
        #[arg(long, conflicts_with = "keep")]
        kindle: bool,
        #[arg(long)]
        keep: bool,
    },
    /// Pair a Kindle: writes pairing.json to copy onto the device
    Pair {
        /// Address the Kindle should connect to
        #[arg(long)]
        host: String,
        #[arg(long, default_value_t = 7419)]
        port: u16,
        #[arg(long, default_value = "pairing.json")]
        out: PathBuf,
    },
    /// Run the foreground TLS sync listener
    Serve {
        #[arg(long, default_value = "0.0.0.0:7419")]
        bind: String,
        /// Exit after one landing (testing)
        #[arg(long, hide = true)]
        once: bool,
    },
    /// Print the current flight plan JSON
    Plan,
}

fn home(cli: &Cli) -> Result<PathBuf> {
    let p = match (&cli.home, std::env::var_os("POMO_KNDL_HOME")) {
        (Some(h), _) => h.clone(),
        (None, Some(h)) => PathBuf::from(h),
        _ => dirs::data_dir().context("no data dir")?.join("pomo-kndl"),
    };
    std::fs::create_dir_all(&p)?;
    Ok(p)
}

fn short(id: &str) -> &str {
    &id[..8.min(id.len())]
}

fn main() -> Result<()> {
    let cli = Cli::parse();
    let dir = home(&cli)?;
    let mut db = Db::open(&dir.join("pomo-kndl.db"))?;
    match cli.cmd {
        Cmd::Add { title } => {
            let id = db.add(&title)?;
            println!("added {}", short(&id));
        }
        Cmd::Edit { id, title } => {
            let id = db.resolve_id(&id)?;
            db.edit(&id, &title)?;
            println!("edited {}", short(&id));
        }
        Cmd::Move { id, position } => {
            let id = db.resolve_id(&id)?;
            db.move_to(&id, position)?;
            println!("moved {}", short(&id));
        }
        Cmd::Tasks { all } => {
            let ts = db.tasks(false)?;
            if ts.is_empty() {
                println!("no tasks");
            }
            for t in ts.iter().filter(|t| all || t.status == "open" || t.status == "done") {
                let mark = if t.status == "done" { "x" } else { " " };
                let rev = if t.review { "  [review]" } else { "" };
                println!("[{mark}] {}  {}{rev}", short(&t.id), t.title);
            }
        }
        Cmd::Done { id } => {
            let id = db.resolve_id(&id)?;
            db.set_status(&id, "done")?;
            println!("done {}", short(&id));
        }
        Cmd::Reopen { id } => {
            let id = db.resolve_id(&id)?;
            db.set_status(&id, "open")?;
            println!("reopened {}", short(&id));
        }
        Cmd::Archive { id } => {
            let id = db.resolve_id(&id)?;
            db.set_status(&id, "archived")?;
            println!("archived {}", short(&id));
        }
        Cmd::Config { focus, brk } => {
            if focus.is_some() || brk.is_some() {
                db.set_config(focus, brk)?;
            }
            println!(
                "focus {} min, break {} min (applies from the next Kindle session after landing)",
                db.meta_i64("focus_min")?,
                db.meta_i64("break_min")?
            );
        }
        Cmd::History => history(&db)?,
        Cmd::Status => status(&db)?,
        Cmd::Resolve { id, kindle, keep } => {
            if !kindle && !keep {
                anyhow::bail!("choose --kindle or --keep");
            }
            let id = db.resolve_id(&id)?;
            let n = db.resolve(&id, kindle)?;
            println!("resolved {n} request(s) for {}", short(&id));
        }
        Cmd::Pair { host, port, out } => {
            let ident = net::identity(&dir)?;
            let token = hex::encode(rand::random::<[u8; 32]>());
            let dev = sync::add_device(&db, &token)?;
            let p = serde_json::json!({
                "proto": sync::PROTO, "host": host, "port": port,
                "server_sha256": ident.fingerprint(), "device_id": dev, "token": token,
            });
            net::write_private(&out, serde_json::to_string_pretty(&p)?.as_bytes())?;
            println!(
                "paired device {}. Copy {} to the Kindle app folder (extensions/pomo-kndl/), then delete it here.",
                short(&dev),
                out.display()
            );
        }
        Cmd::Serve { bind, once } => net::serve(&mut db, &dir, &bind, once)?,
        Cmd::Plan => println!("{}", serde_json::to_string_pretty(&sync::plan(&db)?)?),
    }
    Ok(())
}

fn history(db: &Db) -> Result<()> {
    let mut st = db.conn.prepare(
        "SELECT local_date, COALESCE(title,'?'), status, focus_sec FROM entries
         WHERE kind='session' ORDER BY local_date, seq",
    )?;
    let rows: Vec<(String, String, String, i64)> = st
        .query_map([], |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?)))?
        .collect::<Result<_, _>>()?;
    if rows.is_empty() {
        println!("no sessions landed yet");
    }
    let mut day = String::new();
    for (d, t, s, f) in &rows {
        if *d != day {
            day = d.clone();
            println!("{d}");
        }
        println!("  {:>3} min  {s:<11} {t}", f / 60);
    }
    let mut st = db.conn.prepare(
        "SELECT COALESCE(title,'?'), COUNT(*), SUM(focus_sec) FROM entries
         WHERE kind='session' GROUP BY task_id ORDER BY SUM(focus_sec) DESC",
    )?;
    let per: Vec<(String, i64, i64)> = st
        .query_map([], |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)))?
        .collect::<Result<_, _>>()?;
    if !per.is_empty() {
        println!("per task");
        for (t, n, f) in per {
            println!("  {n} sessions, {} min  {t}", f / 60);
        }
    }
    Ok(())
}

fn status(db: &Db) -> Result<()> {
    let pv = db.meta_i64("plan_version")?;
    println!("plan version {pv}");
    let mut st = db.conn.prepare(
        "SELECT id, acked_seq, plan_version, last_landing, clock_offset FROM devices",
    )?;
    let devs: Vec<(String, i64, i64, Option<i64>, Option<i64>)> = st
        .query_map([], |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?, r.get(4)?)))?
        .collect::<Result<_, _>>()?;
    if devs.is_empty() {
        println!("no Kindle paired (run `pomo-kndl pair --host <ip>`)");
    }
    for (id, seq, v, last, off) in devs {
        let when = match last {
            Some(t) => format!("{} min ago", (sync::now() - t) / 60),
            None => "never".into(),
        };
        let stale = if v < pv { " (newer plan waiting)" } else { "" };
        println!(
            "kindle {}: last landing {when}, carries plan {v}{stale}, log acked to seq {seq}, clock offset {}s",
            short(&id),
            off.map(|o| o.to_string()).unwrap_or("?".into())
        );
    }
    let c = db.conflicts()?;
    if c.is_empty() {
        println!("nothing to review");
    } else {
        println!("needs review (pomo-kndl resolve <id> --kindle|--keep):");
        for (_, tid, title, kind) in c {
            println!("  {}  Kindle asked '{kind}': {title}", short(&tid));
        }
    }
    Ok(())
}
