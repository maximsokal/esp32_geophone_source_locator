# Legacy control aliases and optional-cache fail-safe

- Evidence: the continuation audit at HEAD e41bea8456c4ccc3458956910b572648ce63f5f0 found direct legacy /api/control recorder actions, a STOP-to-session-list race during cache invalidate/publish, and a status-cache START guard that could reject local/TFT recording after cache initialization or refresh failure. The corrected tree routes both legacy actions through measurement_service, waits on fast storage_busy status before the session GET, and allows START when the cache is unavailable while retaining refresh/START arbitration.
- Root cause: compatibility HTTP actions had kept an older recorder path beside the canonical measurement state machine, while optional observability/cache readiness had been treated as a mandatory control admission condition. The React STOP path also assumed REVIEW implied that the asynchronous cache publish had completed.
- Recurrence count: one confirmed episode in this task; no generalized rule is proposed from this single occurrence.
- Proposed minimal rule: compatibility commands must delegate to the canonical lifecycle service; optional status/cache failures must degrade cache-backed WebUI reads without disabling device-local control; and every asynchronous STOP-to-list flow needs an explicit publish/idle gate.
- Affected prior task: the 2026-09-10 measurement-service/status-cache arbitration continuation in this repository.
- Verification: the final documented suite passed (SUITE_RESULT: PASS), with no flash, serial monitor, HIL, or physical verification.
