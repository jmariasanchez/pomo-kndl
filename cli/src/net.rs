//! TLS identity and foreground sync listener.
use crate::db::Db;
use crate::sync;
use anyhow::{Context, Result};
use rustls::pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer};
use sha2::{Digest, Sha256};
use std::io::{BufRead, BufReader, Read, Write};
use std::net::TcpListener;
use std::path::Path;
use std::sync::Arc;
use std::time::Duration;

const MAX_LINE: u64 = 4 * 1024 * 1024;

pub struct Identity {
    pub cert: Vec<u8>,
    pub key: Vec<u8>,
}

impl Identity {
    pub fn fingerprint(&self) -> String {
        hex::encode(Sha256::digest(&self.cert))
    }
}

/// Load or create the persistent self-signed server identity.
pub fn identity(dir: &Path) -> Result<Identity> {
    let cp = dir.join("server.der");
    let kp = dir.join("server.key.der");
    if cp.exists() && kp.exists() {
        return Ok(Identity {
            cert: std::fs::read(&cp)?,
            key: std::fs::read(&kp)?,
        });
    }
    let ck = rcgen::generate_simple_self_signed(vec!["pomo-kndl".into()])?;
    let id = Identity {
        cert: ck.cert.der().to_vec(),
        key: ck.key_pair.serialize_der(),
    };
    write_private(&kp, &id.key)?;
    std::fs::write(&cp, &id.cert)?;
    Ok(id)
}

pub fn write_private(p: &Path, data: &[u8]) -> Result<()> {
    let tmp = p.with_extension("tmp");
    {
        let mut o = std::fs::OpenOptions::new();
        o.write(true).create(true).truncate(true);
        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt;
            o.mode(0o600);
        }
        let mut f = o.open(&tmp)?;
        f.write_all(data)?;
        f.sync_all()?;
    }
    std::fs::rename(&tmp, p)?;
    Ok(())
}

pub fn serve(db: &mut Db, dir: &Path, bind: &str, once: bool) -> Result<()> {
    let id = identity(dir)?;
    let _ = rustls::crypto::ring::default_provider().install_default();
    let cfg = rustls::ServerConfig::builder()
        .with_no_client_auth()
        .with_single_cert(
            vec![CertificateDer::from(id.cert.clone())],
            PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(id.key.clone())),
        )?;
    let cfg = Arc::new(cfg);
    let l = TcpListener::bind(bind).with_context(|| format!("bind {bind}"))?;
    eprintln!(
        "pomo-kndl serve: listening on {} (fingerprint {})",
        l.local_addr()?,
        &id.fingerprint()[..16]
    );
    for s in l.incoming() {
        let mut s = match s {
            Ok(s) => s,
            Err(e) => {
                eprintln!("accept: {e}");
                continue;
            }
        };
        let peer = s.peer_addr().map(|a| a.to_string()).unwrap_or_default();
        s.set_read_timeout(Some(Duration::from_secs(30)))?;
        s.set_write_timeout(Some(Duration::from_secs(30)))?;
        let r = (|| -> Result<String> {
            let mut conn = rustls::ServerConnection::new(cfg.clone())?;
            let mut tls = rustls::Stream::new(&mut conn, &mut s);
            let mut line = String::new();
            BufReader::new((&mut tls).take(MAX_LINE)).read_line(&mut line)?;
            let reply = sync::handle(db, line.trim_end());
            let summary = format!(
                "{} acked_seq={}",
                reply["type"].as_str().unwrap_or("?"),
                reply["acked_seq"]
            );
            let mut out = serde_json::to_vec(&reply)?;
            out.push(b'\n');
            tls.write_all(&out)?;
            tls.flush()?;
            tls.conn.send_close_notify();
            let _ = tls.flush();
            Ok(summary)
        })();
        match r {
            Ok(m) => eprintln!("landing from {peer}: {m}"),
            Err(e) => eprintln!("landing from {peer} failed: {e}"),
        }
        if once {
            break;
        }
    }
    Ok(())
}
