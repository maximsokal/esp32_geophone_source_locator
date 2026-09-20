# Candidate retrospective: bare ESP32 still brownouts at PHY

- Evidence: COM4 attachment `9f338eb0-e5c5-4cbf-a00e-cc12c8112225` was captured with the ESP32 board removed from the device. The reduced image contains no display, SD, recovery, AD7606, INA3221, TCA, or service initialization; it reaches `phy_init: phy_version` and immediately logs BOD. `STA PHY bring-up complete` never appears.
- Correction: external device wiring and the connected peripheral modules are no longer plausible causes of this minimal run. The earlier startup-load hypothesis is closed by both the reduced image and the physically removed-board control.
- Remaining software candidate: the application-specific boundary is the standard `esp_wifi_set_mode(WIFI_MODE_STA)` followed by `esp_wifi_start()`; `CONFIG_ESP_PHY_ENABLE_USB=y` keeps the USB PHY enabled during Wi-Fi on ESP32-S3 and remains untested as a configuration A/B.
- Recurrence count: the same PHY/BOD boundary now survives removal of application workload and removal of the device wiring. Later BODs before the Wi-Fi marker are secondary low-rail retries, not separate root locations.
- Proposed minimal rule: stop editing SD/LVGL/recovery code. Test the same bare board with USB PHY disabled; if unchanged, run a stock/minimal ESP-IDF Wi-Fi station image or scope the ESP32 3V3 rail and substitute the DevKit/power path.
