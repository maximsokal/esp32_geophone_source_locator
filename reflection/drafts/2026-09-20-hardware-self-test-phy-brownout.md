# Candidate retrospective: standalone ESP32-S3 self-test reaches PHY brownout

- Evidence: the standalone `tests/target/hardware_self_test` image was flashed to COM4 and its log passed chip/ROM/MAC, flash RDID/size, PSRAM runtime/memtest, heap integrity, NVS, netif, and Wi-Fi driver initialization. It then logged `WIFI_PHY_START_BEGIN`, `phy_init: phy_version ...`, and `E BOD`; no scan or SoftAP could start.
- Configuration correction: the generated diagnostic config had `CONFIG_ESP_PHY_ENABLE_USB` disabled, so the unchanged failure is not explained by the ESP32-S3 USB-PHY option. The generated config retained `CONFIG_ESP_PHY_MAX_TX_POWER=20` despite the default file requesting 10; do not claim a 10 dBm A/B from this run.
- Recurrence count: the same PHY/BOD boundary is present in the production image and the standalone image, and now survives removal of application peripherals plus the USB-PHY A/B.
- Safety decision: do not erase PHY calibration NVS implicitly. Treat the calibration-erase branch as a separate authorized write test; preserve NVS in the completed diagnostic run.
- Proposed minimal rule: once a stripped image reaches a fixed ESP-IDF boundary with all prerequisites passing, stop adding application-level logging and use a second physical board or an oscilloscope/current capture to separate software from rail/board failure.
