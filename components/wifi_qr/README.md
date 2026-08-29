# wifi_qr migration note

The former project-local QR renderer drew RGB565 bitmaps directly through the custom display renderer.

Stage 11D moved the TFT UI to LVGL 9 + Espressif `esp_lvgl_port`. Wi-Fi setup QR rendering now uses LVGL's `lv_qrcode` widget from the UI layer, so this directory intentionally contains no ESP-IDF component/CMakeLists.txt.
