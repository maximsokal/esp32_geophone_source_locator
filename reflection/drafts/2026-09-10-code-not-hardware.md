# Candidate retrospective: explicit code-only correction was initially missed

- Evidence: the user explicitly corrected the workflow to fix the firmware/UI code rather than inspect or flash the board. The real log showed recorder throughput loss (`samples=768`, `drops=3268`) while STOP stages completed with `ESP_OK`.
- Root cause: port/process inspection was allowed to continue after the user had narrowed the task to implementation, delaying the recorder fix.
- Recurrence count: 1 confirmed episode for this exact code-versus-hardware correction; do not generalize yet.
- Minimal proposed change: when the user narrows an embedded task to code, freeze hardware actions and prioritize source changes, host contracts, and no-flash build evidence.
- Affected work: 2026-09-10 recorder throughput and Web UI recording-path correction.
- Validation: host C 28/28, Python 43 passed + 1 HIL skip, Web Console 30/30, Web XR 582/582, typecheck PASS, ESP Unity and production builds PASS; no flash, monitor, HIL, or physical verification.
