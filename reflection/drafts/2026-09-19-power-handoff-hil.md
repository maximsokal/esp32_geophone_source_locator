# Candidate retrospective: explicit ESP32 power handoff before AD7606 HIL

- Evidence: the user clarified that COM4 is sufficient for flashing but not for powering the AD7606; the previous live run started from COM4 power and reported AD7606 `presence=FAIL`. No valid physical conclusion can be drawn from that run.
- Root cause: the HIL runner modeled separate flash and log ports but did not model the required physical power transition and post-handoff EN/RESET.
- Recurrence count: 1 confirmed episode.
- Proposed minimal rule: when the board supply must change after flashing, the runner must flash first, pause for an explicit physical handoff, start a passive receive-only log monitor, and require a post-handoff reset before accepting physical evidence.
- Implemented narrow mitigation: `-WaitForPowerHandoff` in `tools/run_stress_hil.ps1`; monitor cleanup now also terminates its exact child process tree. This is a task-local proposal, not a generalized agent-rule change.
- Affected task: ESP32 stress HIL and AD7606 physical verification.
