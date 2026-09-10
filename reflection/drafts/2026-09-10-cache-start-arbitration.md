# Cache refresh and measurement START arbitration

- Evidence: the objective audit identified a TOCTOU window between Web status-cache refresh and measurement START. A shared arbitration object now reserves refresh or START before SD work, START reservation is held until the measurement worker enters `STARTING`, and a deterministic host interleaving test covers refresh/start rejection and release. Host C verification reached 29/29 after the change.
- Root cause: cache readiness and measurement admission were previously separate boolean/callback decisions, so a concurrent refresh could begin after START had passed its guard but before the worker performed recorder SD operations.
- Recurrence count: 1 confirmed episode in this task; no generalized rule proposed.
- Proposed minimal rule: when two asynchronous subsystems share storage eligibility, reserve the shared resource before the first admission decision and release it at the consumer's actual state transition; require a deterministic interleaving test before target validation.
- Affected prior tasks: the status-cache/runtime-top10 continuation in this repository; no other confirmed task.
