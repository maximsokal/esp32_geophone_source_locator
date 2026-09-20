# Retrospective candidate: startup contract missed boot-time LVGL load

- Evidence: firmware `687a064f` reaches SD mount and all startup services, then repeats `STA PHY bring-up begin` -> `phy_version` -> brownout; `STA PHY bring-up complete` is absent. Runtime telemetry reports CPU0 was already 94% busy after service creation.
- Root cause: the prior contract guarded only `ui_init()` and the dynamic LVGL timer. `ui_boot_init()` still creates the LVGL task before `esp_wifi_start()`, while storage/recovery/service tasks also precede PHY. The code comment therefore described a stronger isolation guarantee than the source implemented.
- Recurrence count: 2 confirmed PHY-stage reproductions across the prior supply/capacitor comparisons; this is now a repeated startup-order hypothesis, not a one-log anomaly.
- Minimal rule proposal: a pre-radio-load contract must cover every task-producing call, including boot UI, storage/recovery workers, service workers, telemetry, and web tasks; testing only the interactive UI entry point is insufficient.
- Affected area: `main/app_startup.c`, `tests/python/test_startup_power_load_contract.py`, and Wi-Fi brownout diagnosis. No production source change was made in this turn.
