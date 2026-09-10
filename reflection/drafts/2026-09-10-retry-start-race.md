# Retry START race correction

- Evidence: `webui/src/features/objects/measurementTransition.ts` returned every `measurement.state === 'ERROR'` as terminal, while `measurement_service_submit()` accepts START from ERROR and holds the submit gate until the worker processes the queued command. The first status poll could therefore observe the previous ERROR with `busy=true`.
- Root cause: transition polling had no distinction between an in-flight accepted retry and a completed operation error.
- Correction: keep polling while measurement `busy` is true; treat ERROR as terminal only once the gate is released. Session refresh policy now applies only to STOP, after REVIEW or terminal ERROR.
- Verification: console tests 36/36, TypeScript typecheck PASS, Python liveness contract 7 passed. Final `tools/run_suite.ps1 -Suite All` passed: Host C 28/28, Python 44 passed + 1 HIL skip, Web Console 36/36, Web XR 582/582, typecheck, Unity build, and production build all PASS; HIL was skipped.
- Recurrence count: 1 confirmed episode.
- Proposed minimal rule: when an async command may be accepted from its terminal/error state, expose and honor an in-flight generation or submit gate before interpreting stale terminal state as the new command result; do not generalize from this single episode without a second confirmed occurrence.
