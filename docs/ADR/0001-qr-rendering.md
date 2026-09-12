# ADR 0001: On-device QR rendering

## Decision

Use LVGL `lv_qrcode` for QR generation on the device TFT.

## Rejected for now

Do not add `espressif/qrcode`.

## Rationale

All current QR consumers are LVGL widgets. `lv_qrcode` already encodes the
payload and renders the QR into the LVGL object, so it keeps encoding and
presentation in the existing UI boundary. A separate encoder would duplicate
the QR path and require manual matrix-to-LVGL rendering.

Re-evaluate this decision when a non-LVGL QR consumer appears.
