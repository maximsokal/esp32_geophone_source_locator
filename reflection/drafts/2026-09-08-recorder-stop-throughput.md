# Recorder STOP timeout and throughput regression

- Date: 2026-09-08
- Evidence: physical log showed STOP at about 34.7 s, a 5 s `seismic_recorder_stop()` timeout, then successful worker finalization about 2 s later; the same run reported 5897 samples and 1584 dropped frames.
- Root cause: a soft wait deadline was returned as `ESP_ERR_TIMEOUT` and the measurement service converted every non-OK result to ERROR while the recorder continued draining. Separately, the recorder consumer can fall behind the 2048-frame ring while 128-frame writes pass through FATFS and shared SPI2/TFT arbitration.
- Change: STOP is now request/completion based with an exact target cursor; service remains STOPPING until worker finalization and metadata commit; real errors are reported by explicit stage and the arbitrary no-progress watchdog was removed. The recorder worker executes `DRAIN -> FLUSH -> SYNC -> CLOSE -> METADATA -> COMPLETE/ERROR` one stage per turn and publishes the first failing stage/error through recorder, measurement-service and status snapshots. Added grep-friendly storage, SPI/TFT, file-operation, batch, cursor, backlog and drop timing markers.
- Verification: host C 21/21; focused recorder Python contracts 4/4; full Python 27 passed/1 skipped; ESP-IDF 5.5.4 production build PASS; refreshed `-fstack-usage` build PASS with 1820 `.su` files. No flash or physical HIL.
- Recurrence count: one confirmed physical STOP/state-machine episode and one confirmed physical throughput episode in the supplied log; insufficient evidence for a generalized process rule.
- Proposed minimal rule: asynchronous stop APIs must separate request, progress, completion and hard-stall states; tests must model completion after the old timeout and assert STOPPING/REVIEW; throughput fixes must be based on per-stage timing evidence rather than increasing a total timeout.
- Affected prior work: the earlier nonblocking recorder implementation (`a5a2def`), measurement service async state machine (`290adca`), batched recorder writes (`226c3a1`), shared SPI2 arbitration (`dfab355`), and the current stack/path refactor in `3439060`.

## Follow-up correction

- Evidence: review rejected the intermediate `SEISMIC_RECORDER_STOP_STALL_TIMEOUT_MS 15000U` watchdog as another arbitrary wall-clock limit.
- Root cause: the first fix separated the service from the recorder wait but still modeled STOP completion as a timeout-protected monolith instead of an explicit FreeRTOS worker state machine.
- Correction: remove the timeout entirely; execute each finalization stage as a recorder-task turn, preserve the first failing stage/error, and publish `COMPLETE` or `ERROR` only after the chain ends.
- Recurrence count: one review correction; retain as a local lesson, not a generalized rule yet.
- Proposed minimal rule: do not add or increase a STOP timeout when the operation is designed to complete asynchronously; model each externally-visible sub-operation as a state transition with propagated status.
