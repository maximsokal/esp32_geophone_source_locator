# Candidate retrospective: independent HIL parser measurement gate

- Evidence: the HIL parser accepted a target ISR line by matching `status=PASS` alone. It now extracts `ticks` and `expected_min` and requires the measured count to meet the threshold, with a source contract covering both comparisons.
- Root cause: the parser trusted a target-produced status token instead of independently validating the numeric invariant.
- Recurrence count: 1 confirmed episode in this task.
- Proposed minimal rule: replay/HIL parsers should validate measured numeric fields independently of target verdict strings whenever the log exposes the required values.
- Affected task: ESP32 stress HIL parser and future telemetry-backed acceptance gates.
