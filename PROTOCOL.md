# pomo-kndl sync protocol v1

Transport: TLS 1.2+ over TCP, default port 7419. One landing per connection. Each message is one line of UTF-8 JSON terminated by `\n`. Maximum message size 4 MiB.

## Pairing

`pomo-kndl pair --host <computer-ip>` on the computer:

1. Creates the server TLS identity (self-signed, persistent) if missing.
2. Creates a device record with a random 256-bit token.
3. Writes `pairing.json`:

```json
{"proto":1,"host":"192.168.1.20","port":7419,"server_sha256":"<hex of DER cert>","device_id":"<uuid>","token":"<hex>"}
```

Copy `pairing.json` to the Kindle app directory once (USB). The Kindle pins `server_sha256` and rejects any other certificate. The computer rejects unknown device IDs or tokens.

## Landing

Kindle sends:

```json
{"type":"land","proto":1,"device_id":"...","token":"...","plan_version":12,"clock":1777000000,"entries":[ENTRY...]}
```

`ENTRY` (all have `id` uuid, `seq` device sequence starting at 1, `plan_version` the version the Kindle carried, `wall` Kindle unix seconds, `kind`):

- `{"kind":"session","task_id":"...","title":"...","planned_min":25,"focus_sec":1490,"status":"completed|interrupted|cancelled","local_date":"2026-10-24"}`
- `{"kind":"break","break_sec":300,"local_date":"2026-10-24"}`
- `{"kind":"done","task_id":"..."}`
- `{"kind":"reopen","task_id":"..."}`

Computer replies after a durable commit:

```json
{"type":"landed","acked_seq":57,"plan":{"version":13,"settings":{"focus_min":25,"break_min":5},"tasks":[{"id":"...","title":"...","position":1,"review":false}]}}
```

or `{"type":"error","code":"auth|proto|bad_entry","message":"..."}`.

## Rules

- Entries already stored (same `device_id` and `id`) are skipped, so redelivery is harmless. `acked_seq` is the highest stored sequence for the device. The Kindle deletes entries with `seq <= acked_seq` and atomically replaces its plan.
- Sessions and breaks are always accepted.
- `done`/`reopen` apply only if the task's last computer change is at or before the entry's `plan_version`. Otherwise they are stored as conflicts, the task shows `review: true`, and `pomo-kndl resolve` settles it.
- `wall` and `clock` are informational (clock offset, display grouping) and never decide ordering.
- The plan contains every open (not done, not archived) task.
