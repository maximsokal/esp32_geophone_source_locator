# ESP32-S3 hardware self-test

This is a separate diagnostic target. It does not modify the main application
and does not erase NVS. The default diagnostic build disables the internal USB
PHY during Wi-Fi initialization (`CONFIG_ESP_PHY_ENABLE_USB=n`) so that this
remaining PHY configuration A/B is explicit.

The target reports CPU/chip identity, flash JEDEC/physical size, PSRAM boot and
runtime checks, heap integrity, reset reason, Wi-Fi driver/PHY start, a blocking
2.4 GHz scan, and a continuously advertised open SoftAP. The scan result is
reported by the firmware; SoftAP visibility must be checked by a nearby Wi-Fi
client or host adapter.
