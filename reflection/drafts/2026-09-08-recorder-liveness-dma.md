
## Recorder post-operation liveness and DMA marker, 2026-09-08

- Evidence: `components/seismic_recorder/seismic_recorder.c` now publishes `RUNTIME_TASK_RECORDER` only through `seismic_recorder_observe()` after buffer copy, SD write/flush, finalization, or delay; `components/ad7606_driver/ad7606_driver.c` logs `DMA=auto (SPI_DMA_CH_AUTO)`, and the HIL parser rejects logs without it. Focused tests passed 12/12; full pytest 14 passed/1 skipped; host C 21/21; production build exit 0.
- Root cause: recorder heartbeat was placed at loop entry, before operations that can block on the buffer or SD path, so a status poll could observe a false alive signal. The serial startup line proved SPI initialization but did not expose the DMA selection in captured evidence.
- Recurrence count: one confirmed episode in this task.
- Proposed minimal action: keep task liveness beats at operation completion boundaries and require runtime markers to describe safety-critical transport configuration; add a focused regression whenever either ordering or marker contract changes.
- Affected prior task: staged Measurements pipeline HIL implementation and hardening on 2026-09-08.

## Runtime age gate follow-up, 2026-09-08

- Evidence: runtime snapshots now carry device monotonic `now_ms`; `/api/status` includes it and the live runner checks all five `last_heartbeat_ms` values against `-MaxTaskHeartbeatAgeMs` (default 1000 ms), with a Python stale-keypad fixture proving an advancing heartbeat alone cannot pass. Full pytest was 15 passed/1 skipped and target build exited 0.
- Root cause: comparing only two heartbeat counters proved that a task ran at least once between polls, but could miss a task that stopped immediately afterward; host wall-clock time was not an authoritative device age.
- Recurrence count: one confirmed episode in this follow-up.
- Proposed minimal action: retain a device monotonic timestamp in every runtime snapshot and validate both counter advancement and bounded age in live gates.
- Affected prior task: staged Measurements pipeline HIL implementation and first liveness hardening on 2026-09-08.

## Runtime snapshot clock ordering correction, 2026-09-08

- Evidence: `runtime_observability_get_snapshot()` now reads `esp_timer_get_time()` while holding `s_lock`; Python fixtures reject future heartbeat timestamps and backwards `runtime.now_ms`. Focused HIL/liveness tests passed 15/15, host C 21/21, and target build exited 0.
- Root cause: taking the device clock before entering the table lock allowed a concurrent beat to publish a newer `last_heartbeat_ms` than the snapshot `now_ms`, causing a false future-timestamp failure.
- Recurrence count: one confirmed review finding in this follow-up.
- Proposed minimal action: capture correlated observation clocks and task state under one short lock, with explicit future/backward regression fixtures.
- Affected prior task: runtime age-gate implementation for the staged Measurements pipeline HIL on 2026-09-08.

## Post-operation liveness loop audit, 2026-09-08

- Evidence: source contract covers all five loops. Acquisition beats follow AD7606 read and seismic buffer publication; recorder follows buffer copy/SD write/flush/finalize; keypad follows poll delay; UI now follows event receive and LVGL unlock; power button now follows poll delay. Full pytest passed 21/21 plus one hardware skip; host C 21/21; target build exited 0.
- Root cause: UI and power-button beats were emitted before a potentially blocking receive or expected delay, weakening the runtime age gate's interpretation of task service completion.
- Recurrence count: one confirmed audit episode.
- Proposed minimal action: keep heartbeat calls at service completion boundaries and retain source-order regression coverage for each task loop.
- Affected prior task: staged Measurements pipeline liveness implementation on 2026-09-08.
