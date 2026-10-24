# UI and connection acceptance recheck

Date: 24 October 2026. Changes: `e6bbfca`, `19f02f7`, `8a2baa2`.

## Interpretation and scope audit

The request was to tidy the UI and explain what is wrong with the current "design connection". UI cleanup is explicit. I inferred that "design connection" means the Kindle-to-computer sync design, based on the native GTK application, its networking code, and the previous project discussion. It could instead refer to visual coherence or an external design tool. The user has not explicitly confirmed the inferred meaning.

The initial, uncertain phase performed read-only repository, history, MCP-connection, and environment inspection. No UI or networking edits were made before identifying the project and stating the sync interpretation. The eventual changes preserve the existing monochrome Kindle design rather than introducing a web UI or unrelated design integration. The additional build/test-script changes address failures encountered while verifying the requested UI and connection work.

Subjective visual preference has not been approved by the user. The concrete improvements verified below are task reachability, successful startup refresh, visible failure state, and preservation of offline work.

## Real public-interface exercise

This pass did not use `POMO_KNDL_SCRIPT`, `POMO_KNDL_AUTO_SYNC`, manually constructed state JSON, copied application source, mock servers, or direct internal callbacks. It used the repository's built executables, the actual native macOS GTK window, the public CLI, and the real pinned-TLS server. Application data was isolated in a new scratch directory. No real user tasks or mounted Kindle files were changed.

1. Created eight tasks with `pomo-kndl --home <test-home> add`, then paired using `pair --host 127.0.0.1 --port 17429 --out <test-kindle>/pairing.json` and ran `serve --bind 127.0.0.1:17429`.
2. Launched `kindle/build/pomo-kndl-gui` normally with only `POMO_KNDL_DIR` and `POMO_KNDL_GEOMETRY=618x824`. Both script variables were explicitly unset. Without pressing Sync, the real CLI reported a landing carrying plan 8, and the Kindle saved all eight tasks with zero pending entries.
3. The native window showed page 1/2. Clicked the actual Next target, observed tasks 6, 7 and 8 on page 2/2, then clicked task 8. The visible screen changed to Focusing / Acceptance task 8 / 25 minutes. The persisted session had `phase=focus` and the matching task title. This is an observed improvement over the original seven-task cutoff, which had no navigation to later carried tasks.
4. Stopped only the isolated test server. A subsequent attempted native Pause click did not change state after the user's active application took focus. GTK did not expose usable Accessibility controls on this Mac. To avoid further interference with the live desktop, stopped native pointer testing and closed only the test GUI. Do not count that attempted click as a successful native Pause test.
5. Continued through the shipped public headless client: `pause`, `end`, `done`, and `sync`. The timer recorded 92 seconds of focus and completion of task 8. With the server unavailable, sync returned `computer unreachable at 127.0.0.1:17429`. SHA-256 of the saved state was identical before and after this failed sync. Both the session and completion entries remained pending, and task 8 remained crossed off locally.
6. Restarted the isolated real server and ran the public client `sync`. It returned `landed: plan 9, 0 pending`. The computer's public `tasks` command showed task 8 completed, `history` showed one interrupted session with one whole focus minute, and `status` showed acknowledgement through sequence 2 and nothing to review.
7. Added a new task through the public CLI after that successful landing and moved it to the top. An initial ID-extraction command supplied an invalid prefix and was rejected without applying the move; using the actual returned task ID succeeded. The Kindle still had plan 9, a recorded successful sync and an empty outgoing log before relaunch.
8. Relaunched the actual GUI normally, again with both script variables unset. Without a manual Sync, it fetched plan 11 and displayed **Fresh task after successful sync** as the first task. This directly exercises the startup condition that the original implementation skipped.
9. Stopped the isolated server and relaunched the normal GUI again. The actual window showed **Sync failed**, retained all eight carried tasks and kept the Sync control available. Its saved plan remained 11. This demonstrates a usable offline screen rather than a false current-success indication after a failed attempt.

Native-window evidence, captured from the real application rather than its screenshot hook:

![Normal launch fetched the fresh task](screens/native-launch.png)

![Offline launch retains tasks and reports failure](screens/native-offline.png)

The earlier GTK scripted suite remains complementary evidence for keyboard callbacks, malformed-pairing retries, long-title layout, session transitions and connection-detail navigation. It passed at 618x824 and 1236x1648. It is not being relabeled as a physical-device test. The network suite, real Rust/C++ end-to-end suite, and ARM VM sync tests also passed.

## Current real connection observation

At the first inspection, the mounted Kindle's host matched the Mac's current LAN IP, a real server was listening on port 7419, and an intentionally unauthenticated request passed the pinned TLS check before receiving the expected auth rejection.

A later read-only check at **2026-10-24 21:56:58 UTC** found **no listener on port 7419**, and the previously observed server PID was no longer present. The reason it stopped was not established. All server-start/stop commands in the acceptance exercise targeted the isolated server on port 17429, not the user's real server. The real server was not restarted, because it may have been intentionally stopped. Under the current architecture, LAN sync cannot succeed without `pomo-kndl serve` running on the paired computer.

## Acceptance boundary before authorized installation

- The physical Kindle was mounted over USB. The rebuilt application was not installed or launched on it. No physical e-ink refresh, touch, suspend/resume, or Kindle-origin Wi-Fi acceptance result is available.
- The ARM VM verifies executable startup and TLS interoperability, not the Kindle's actual screen or Wi-Fi environment.
- Background native GTK control was unavailable through macOS Accessibility. Once the user's desktop activity interrupted pointer testing, the rest of the session flow was verified through the public client instead of competing for focus.
- All test-only GUI/server processes were stopped. The user's active application was left undisturbed at cleanup. The device's installed application, pairing and task state were not changed.

The host application and real integration boundaries have concrete passing observations. Physical-device acceptance and the user's aesthetic approval remain unverified, so the overall feedback loop is representative, not closed end-to-end on the target device.

## Later authorized installation

After the recheck, the user explicitly requested a build and installation on the Kindle. At approximately 22:03 UTC on 24 October 2026:

- Freshly cross-built revision `5323558` for ARM hard-float on the Mac. Packaging passed the dynamic GTK/GLib check, and the ARM VM passed state, first landing, and offline-work/reconnect checks.
- Copied the entire existing extension into owner-private local backup storage and verified it byte-for-byte before changing the device: `~/Library/Application Support/pomo-kndl/backups/20261024-NjYqvg/app/`.
- Staged and installed exactly six package files: `bin/pomo-kndl-gui`, `bin/pomo-kndl-dev`, `bin/gtk-probe`, `config.xml`, `menu.json`, and `run.sh`. Independently read back all six SHA-256 checksums and confirmed they matched the build.
- Verified every existing non-package file against the backup, including `pairing.json`, `state.json`, runtime libraries and logs. They were unchanged. Verified that the GUI binary differs from the previous installation.
- Installed GUI SHA-256: `18738b9b4a2eae597bc64a665472b8835e9fc4d1d4ca9f83d766638cb9d7160a`.
- Flushed writes and safely ejected the Kindle using `diskutil eject`. No physical KUAL launch or touch/e-ink/Wi-Fi observation was possible from the USB installation path. That runtime acceptance remains pending.

The Mac's port-7419 listener was still absent at installation verification. It was not restarted as part of the device installation. The installed app can work offline; LAN sync still requires the paired computer's `serve` process.

## Whole-result verification after installation

Rechecked the complete result on 24 October 2026, 22:06-22:12 UTC, at revision `cc29e37`. Changes after the installed source revision `5323558` were documentation only. Requirements were mapped before this rerun, rather than relying on earlier checks of individual edits. Local logs, screenshots and the public-interface driver are retained in `~/.jcode/scratch/pomo-final-verification.DDcoNr/`; generated task data is isolated from the user's real database and Kindle state.

| Requirement or changed output | Check rerun over the final result | Observed result and boundary |
| --- | --- | --- |
| Every carried task reachable | `tests/ui.sh` at 618x824 and 1236x1648 | Navigated all three pages, selected task 8, completed it, and asserted exactly one matching completion entry. Both runs passed. |
| Long titles do not displace actions/footer | Both GTK runs, including a 200-character unbroken title | Focus and ended-session allocations stayed within parent bounds. Full-title and review-badge cases passed. Fresh screenshots show separated action rows and footer. |
| Session controls and keyboard activation | Both GTK runs | Focus, Pause, Resume, End session, Complete task, Keep open, break/end-break, reopen, and details/back transitions passed through dispatched GTK events. Enter activation passed. This is host GTK, not physical Kindle touch. |
| First-run and malformed pairing stay recoverable | Both GTK runs | No-tasks screen retained sync access; missing pairing showed guidance; malformed pairing allowed repeated retry without remaining stuck in syncing. |
| Honest, inspectable sync status | Both GTK runs against an actual Rust server | Successful landing showed Last sync OK, completion showed two pending changes, landing reduced the count to zero, stopped-server retries showed Sync failed while retaining tasks. Endpoint, guidance, Back and Sync now remained visible. |
| Startup refresh despite previous success and no outgoing log | Normal GUI launch with both script environment variables unset, actual CLI-created tasks and actual TLS server | First launch imported eight tasks. After a successful landing and a new computer-side task, relaunch fetched the new task with an empty outgoing log. No state fixture or GUI callback hook was used for this check. |
| Offline work survives failed sync and reconnects correctly | Shipped headless client commands against isolated real data/server | Started task 8, paused/resumed/ended/completed it. Two entries remained pending. With the server stopped, sync returned computer unreachable and state SHA-256 was byte-identical before/after. Restarted server acknowledged both entries: plan 9, zero pending; public tasks showed completion and history showed the interrupted session. |
| Existing sync authority/conflict behavior remains intact | `tests/e2e.sh` and `cargo test --manifest-path cli/Cargo.toml` | Real TLS task delivery, rename, completion, history, conflict, resolution and repeated delivery checks all passed; 15 Rust tests passed. |
| Pin/authentication failures remain rejected | Real TLS E2E and network suites | Wrong pin and bad token rejected. Network test confirmed wrong pin rejection before sending the request. |
| TCP attempts bounded without breaking TLS | `make -C kindle net-test CXX=/usr/bin/c++` | Seven groups passed: input validation, refusal, interrupted-connect deadline, blocking-flag restoration, stalled TLS, pinned exchange and wrong pin. Interrupted-connect simulation is explicitly a synthetic supplement; DNS and total-exchange deadline limitations remain. |
| Durable timer/core behavior retained | `make -C kindle test CXX=/usr/bin/c++` | Seven core tests passed. Fake-clock cases are unit evidence, not a physical sleep/resume result. |
| Broken linkage checking fails closed | `READELF=/usr/bin/false bash kindle/cross.sh` | Build exited nonzero; all six existing package files still matched the installed-file manifest. A normal rebuild then passed. |
| ARM output interoperates with the real server | Fresh `kindle/cross.sh`, then `tests/kindle-vm.sh` | VM printed `RESULT state 0`, `RESULT sync1 0`, `RESULT sync2 0`; server-side completion/history checks passed. This is ARM headless execution, not an ARM GUI/display test. |
| Rebuilt output is exactly what was installed | SHA-256 check of all six fresh package files against the saved installation readback manifest, then fresh readback after the device remounted | All six matched in both places, including the GUI hash recorded above. At 22:12 UTC the actual mounted device independently passed all six checksum checks again. |
| KUAL package is coherent | `sh -n`, executable checks, parsed nested menu entries, post-install device log compared with backup | Launcher syntax passed, all binaries and launcher executable, five actions correctly route to `run.sh` with open/stop/sync/diagnose/probe parameters. New device log entries record a sync invocation and a GUI invocation followed by exit 0. Display/touch behavior is not established by that log. |
| Original work and rollback preserved | Prior actual-device byte comparisons; current private-backup permission check | Installation preserved all original non-package files. Backup directory remains mode 0700. No device files were changed during this rerun. |
| Test assertions themselves fail honestly | Both GTK suites and `shellcheck tests/ui.sh tests/e2e.sh` | Missing GTK target/text produced failure as required; shellcheck passed. |

The first scratch-driver attempts exposed verification mistakes, not application failures: a menu assertion inspected the outer menu instead of its five nested actions, shellcheck required quoting the literal `done` command, and a background shell-function PID did not terminate its server child. The driver was corrected to track the executable PID directly, the identified test-only orphan was stopped, and the entire no-hook sequence passed with fresh state. The unsuccessful offline attempt is not counted as evidence of offline preservation. No production process was stopped.

### Current connection observation and stop condition

At **22:06:51 UTC**, a read-only check found the real Mac server **listening on port 7419**, PID 3249. This supersedes the earlier absent-listener observation. It was neither started nor stopped by this recheck. A listener alone does not establish successful Kindle-origin Wi-Fi sync.

During final cleanup the Kindle had remounted. A read-only check at **22:12 UTC** verified all six actual installed-file checksums again. Comparing its log with the pre-install backup found new records: `run.sh sync` at **22:05:26 UTC** failed with computer unreachable, and `run.sh` at **22:05:29 UTC** was followed by `gui exit 0`. This is direct evidence that the installed launcher and GUI process ran, not proof of correct visible rendering or touch input. The backup log contained only the earlier 19:16 invocation.

The pairing remains byte-identical to backup, and its endpoint matches the Mac's current LAN address and port. The current server process started at **22:06:24 UTC**, after the failed device sync recorded at 22:05:26. That ordering is consistent with the server not yet running during the failed attempt; it does not prove the Kindle can currently reach the server. The Kindle retains two tasks and one pending entry. This recheck did not submit that real pending work, overwrite state, restart the production server or eject the newly reattached device.

Concrete improvements are observed, not inferred solely from code: later tasks are selectable; long-title controls fit; startup fetches new work after a prior successful landing; failed sync preserves pending work; reconnect lands completion/history. The rebuilt package is identical to the actual installed package, and a post-install device GUI process exited successfully. These checks cover the host/public interfaces, TLS boundary, ARM headless runtime, packaging and device launcher evidence, but do not close the physical display/touch/Wi-Fi acceptance loop. No remote device execution channel is available. Visible rendering, touch, e-ink behavior, wake behavior, successful post-install Kindle-origin sync and subjective design approval remain explicitly unverified. Repeating host checks cannot establish those results.
