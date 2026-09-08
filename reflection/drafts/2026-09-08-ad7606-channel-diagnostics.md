# Candidate retrospective: AD7606 diagnostics channel coverage

- Evidence: `ad7606_acquisition.c` used `SEISMIC_CHANNEL_COUNT` for diagnostic state and all diagnostic loops even though `ad7606_driver_read()` fills `AD7606_CHANNEL_COUNT` words. The HIL parser and fixture independently required only CH1..CH4.
- Root cause: diagnostics were coupled to the downstream four-channel application contract instead of the ADC transport channel count; parser tests mirrored the same incomplete assumption.
- Recurrence count: 1 confirmed episode in this task. This is insufficient evidence for a generalized repository rule.
- Minimal proposed rule: when a transport exposes more channels than a downstream consumer, keep diagnostic/validation loops keyed to the transport count and add a regression that exercises the highest channel; keep the consumer count explicit at its boundary.
- Affected verification: CH1..CH8 replay parsing, all-eight raw-word host assertion, focused pytest 6/6, native host 21/21, PowerShell AST, and production Ninja build passed after correction.
