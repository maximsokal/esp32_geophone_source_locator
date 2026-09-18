# LVGL config migration: candidate retrospective

- Evidence: the project moved from LVGL `9.5.0` to `9.6.0~1`, while the ignored generated `sdkconfig` retained non-default compatibility values from the older Kconfig. The first post-change ESP-IDF build emitted LVGL deprecation warnings even though compilation still passed.
- Root cause: the tracked defaults did not express the LVGL 9.6 byte-sized memory setting and did not clear the old assert include. The generated config therefore preserved the old values across reconfigure.
- Correction: declare the current LVGL values in `sdkconfig.lvgl.defaults` and migrate only active legacy values in the canonical IDF wrapper. Keep zero/empty generated defaults to make the migration idempotent.
- Verification: ESP32-S3 build `2506/2506`, incremental build `47/47`; verification logs had zero compiler/LVGL warnings and zero errors. Python suite `441 passed, 1 skipped`.
- Recurrence count: one confirmed episode. Proposed minimal rule: whenever a managed component is upgraded, compare its Kconfig rename/deprecation entries against the generated sdkconfig and add a focused migration/idempotence test before declaring the build-clean result.
- Scope: proposal only; do not change `AGENTS.md`, `TOOLS.md`, or a shared skill without separate approval.
