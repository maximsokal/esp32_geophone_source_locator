# Real-board HIL baselines

This directory stores **measured hardware baselines**, not parser fixtures.

## Storage/SPI baseline workflow

1. Start from `storage-spi-baseline.template.json`.
2. Capture UART telemetry from the real ESP32-S3 using the normal hardware workflow. Flashing/programming remains on COM4; passive UART logging remains on COM6.
3. Run the recording/storage scenario under the exact configuration being accepted:
   - actual SD card;
   - actual SDSPI clock;
   - actual seismic sample rate;
   - actual enabled channel count;
   - exact firmware commit being measured.
4. Derive and justify the seven acceptance limits from the measured run(s). Do **not** copy synthetic values from parser unit tests.
5. Copy the template to a dated/config-specific file, for example:
   `storage-spi-2026-09-13-20mhz-500hz-4ch.json`.
6. Replace every `null` with the measured metadata/limits. Keep the raw UART log separately as supporting evidence.
7. Analyze the saved UART log using the measured profile:

```powershell
python .\tools\analyze_storage_baseline.py `
  .\build\hil\storage-spi-<timestamp>.log `
  .\docs\hil-baselines\storage-spi-<measured-profile>.json
```

The template is intentionally invalid until all required measured fields are filled. `tools/analyze_storage_baseline.py` rejects missing metadata, missing/null limits, unknown limit keys, invalid ranges, and channel counts outside the AD7606 width.

## Required baseline metadata

Each measured profile records:

- `measured_at` — timestamp/date of the measurement;
- `board` — tested controller/hardware identity;
- `sd_card` — tested physical SD card/model/identifier;
- `sdspi_hz` — actual configured SDSPI clock;
- `sample_rate_hz` — tested acquisition sample rate;
- `channels` — tested active/required diagnostic channel count (`1..8`);
- `firmware_commit` — exact firmware revision under test;
- `limits_us` — all seven measured/justified latency limits;
- optional `notes` — scenario details or rationale.

## Evidence boundary

A committed JSON profile is configuration metadata, not proof by itself. MAX-10 can be accepted only when the profile is derived from the real board and a corresponding real-board UART capture passes the analyzer with healthy acquisition and zero recorder drops.
