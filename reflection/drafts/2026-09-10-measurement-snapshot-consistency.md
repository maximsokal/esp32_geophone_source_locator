# Measurement snapshot state/busy consistency

- Evidence: `measurement_service_get_snapshot()` copied `s_snapshot` under `s_mutex` but read `measurement_service_is_busy()` after unlocking. A worker transition could therefore expose stale `ERROR` with a gate state from a later phase.
- Root cause: the state and submit-gate reads were not one critical section, even though the WebUI now uses `busy` to distinguish an in-flight retry from a terminal error.
- Correction: read `snapshot->busy` before releasing `s_mutex`; add a host hook that attempts a non-blocking mutex acquisition during `uxSemaphoreGetCount()` and a Python lock-order contract.
- Verification: host C 28/28, focused Python contract 8 passed, full suite `SUITE_RESULT: PASS` with Python 45 passed + 1 HIL skip, Web Console 36/36, Web XR 582/582, TypeScript, Unity, and production builds passing; `git diff --check` clean apart from expected line-ending notices.
- Recurrence count: 1 confirmed episode.
- Proposed minimal rule: any status field derived from a gate, queue, or worker phase must be sampled under the same lock as the state it qualifies; preserve a deterministic interleaving regression when such a pair controls terminal-state interpretation.
