# Candidate retrospective: runtime gates needed target evidence

- Evidence: host and production builds were green while physical CPU/heap/HWM
  values and WebUI transport latency were still unavailable; the HIL runner
  previously accepted only coarse asset markers and did not report heap-floor
  minima.
- Root cause: compile-time policy, asynchronous runtime diagnostics and the
  live WebUI/HIL acceptance checks were not expressed as one explicit contract.
- Recurrence count: 1 confirmed episode; this is not yet evidence for a
  generalized rule change.
- Minimal proposal: every embedded memory/performance change should add a
  build guard, a nonblocking target snapshot path, and a live HIL check that
  reports measured minima/latency separately from host-test evidence.
- Affected work: current ESP32-S3 memory/runtime observability follow-up.

This is a proposal for review. `AGENTS.md`, `TOOLS.md`, and skills were not
modified automatically.
