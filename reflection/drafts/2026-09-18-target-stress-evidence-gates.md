# Candidate retrospective: target stress evidence gates

- Evidence: review of the standalone stress app found that `TARGET_STRESS_ISR status=PASS` only represented successful timer creation/start, not observed callback cadence. The gate now compares `isr_ticks` with a 90%-of-expected minimum, and a source contract protects that comparison.
- Root cause: target telemetry status was coupled to infrastructure setup instead of the measured runtime invariant.
- Recurrence count: 1 confirmed episode in this task.
- Proposed minimal rule: every target stress PASS marker must be derived from measured runtime counters and an explicit threshold; setup success must be reported separately.
- Affected task: ESP32 target stress/HIL validation and future target telemetry contracts.
