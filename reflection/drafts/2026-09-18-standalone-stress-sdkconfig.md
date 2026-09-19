# Candidate retrospective: standalone target SDK defaults

- Evidence: the first standalone `tests/target/stress_app` build inherited the repository production defaults and attempted to resolve a custom `tests/target/stress_app/partitions.csv`, which does not belong to the isolated stress project. The build was corrected by removing the production defaults from the target project, adding a local 16 MB/PSRAM/ISR profile, and explicitly requiring `esp_psram`.
- Root cause: the standalone ESP-IDF project boundary was not explicit about which root-level `sdkconfig.defaults` and partition-table assumptions were production-only.
- Recurrence count: 1 confirmed episode in this task.
- Proposed minimal rule: every standalone ESP-IDF test target must own its sdkconfig defaults and partition-table assumptions; add a contract test that forbids accidental inclusion of production defaults and checks required target-local Kconfig markers.
- Affected task: ESP32 target stress application and future standalone target/HIL projects.
