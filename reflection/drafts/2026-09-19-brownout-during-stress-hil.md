# Candidate retrospective: brownout during post-handoff stress HIL

- Evidence: after the correct COM4-to-USB-18650 power move, COM6 captured four `E BOD: Brownout detector was triggered` markers and `rst:0xf (BROWNOUT_RST)` in `build/hil/stress-target-20260919-002906.log`; no `TARGET_STRESS_SUMMARY` appeared.
- Root cause: not yet isolated to the USB 18650 source, wiring, shared load, or firmware load. The only confirmed conclusion is that the board could not sustain this powered run without brownout.
- Recurrence count: 1 confirmed episode.
- Proposed minimal rule: treat any repeated brownout before `TARGET_STRESS_SUMMARY` as invalid physical HIL evidence; abort with an explicit `BROWNOUT_ABORT` marker and never disable the brownout detector to force a pass. Diagnose supply/current/wiring separately before rerunning.
- Implemented narrow mitigation: automatic completion marker, brownout abort/reporting, and a visible full-screen TFT diagnostic pattern. This is a task-local proposal, not a generalized agent-rule change.
- Affected task: ESP32 stress HIL, AD7606 physical verification, and TFT bring-up.
