# Storage / SPI2 / recorder latency HIL

`tools/run_storage_spi_hil.ps1` is the numerical live-board gate for the shared TFT + microSD SPI2 path and recorder write latency.

## Acceptance state

**PERFORMANCE_ACCEPTANCE: NOT_ESTABLISHED** until a real-board baseline is captured and reviewed.

The threshold values used by `tests/python/test_storage_spi_hil_runner.py` are synthetic parser fixtures only. They prove exact-boundary/pass/fail classification. They are **not** acceptable production thresholds and must never be copied into a hardware acceptance report without a measured baseline.

## What the runner gates

A valid live capture must contain all of the following:

- `STORAGE_SPI_TIMING stage=begin` with `mutex_wait_us`, `tft_quiesce_us`, and a successful result;
- `STORAGE_SPI_TIMING stage=end` with `tft_resume_us` and `elapsed_us`;
- `RECORDER_IO_TIMING op=fwrite` with requested/written bytes, storage-window count, maximum window, `fwrite_us`, total operation time, and successful result;
- positive-sample `AD7606_DIAG` evidence for the required acquisition channels;
- recorder `STOP_COMPLETE` evidence;
- `drops=0`, final stage `complete`, and `err=ESP_OK`;
- no AD7606 read failure, seismic-buffer push failure, acquisition-initialization failure, panic, watchdog, brownout, heap-corruption, abort or assertion marker.

A low latency number with dropped frames or unhealthy acquisition is a **FAIL**, not a performance pass.

Replay with `-LogPath` can validate the parser against a saved log, but `STORAGE_SPI_HIL_RESULT` remains `BLOCKED` unless the runner is capturing a live target.

## Baseline metadata — required before choosing limits

For every candidate baseline record:

- date/time;
- firmware commit SHA;
- exact ESP32 board;
- SD card make/model/capacity/speed class;
- configured and negotiated SDSPI frequency (the current design target is 20 MHz; record the actual value rather than assuming it);
- TFT + SD shared-SPI2 wiring/profile;
- acquisition source;
- sample rate (current production target: 500 Hz);
- recorder channel/frame configuration actually used by that firmware;
- object/session population on the SD card;
- recording duration and concurrent workload;
- each numerical maximum emitted by the runner;
- acquisition-health result and recorder dropped-frame result.

If the card, SPI clock, firmware data layout, sample rate/channel configuration, or workload materially changes, re-establish the baseline.

## Baseline procedure

1. Build/flash the production firmware through the normal production path.
2. Attach only the passive UART log adapter for the HIL capture (normally COM6). `run_storage_spi_hil.ps1` never flashes firmware.
3. Start healthy AD7606 acquisition.
4. Start a recording.
5. During the capture, exercise representative storage/UI activity instead of leaving the target idle.
6. Stop the recording normally so the UART log contains `STOP_COMPLETE`.
7. Preserve the complete UART log.
8. Run the same log through replay mode to confirm that the parser extracts the same values.
9. Repeat enough real-board runs to identify normal variance and worst-case behavior. Do not define a threshold from one lucky minimum-latency run.
10. Choose limits with explicit engineering margin above the measured healthy worst case, record the rationale, then rerun live with those limits.

The live invocation requires explicit limits because the tool intentionally refuses to invent them. Example shape only:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/run_storage_spi_hil.ps1 \
  -LogPort COM6 \
  -MaxMutexWaitUs <measured-limit> \
  -MaxTftQuiesceUs <measured-limit> \
  -MaxTftResumeUs <measured-limit> \
  -MaxSpiEndElapsedUs <measured-limit> \
  -MaxRecorderWindowUs <measured-limit> \
  -MaxRecorderFwriteUs <measured-limit> \
  -MaxRecorderOperationUs <measured-limit>
```

The values in this example are placeholders by design.

## Interface-migration rule

Do **not** migrate to SDMMC or replace SDSPI merely because single-block SDSPI operations exist. First prove from real measurements that sustained storage throughput or bounded latency is the limiting factor and that it causes recorder backlog/dropped frames or unacceptable shared-SPI2 responsiveness. If acquisition is healthy, `drops=0`, and the measured storage windows remain within an acceptable baseline, an interface migration is not justified by this HIL.

For broader physical SPI2/TFT scenarios, including large scan transfer and keypad responsiveness, also see [`spi2-contention-hil.md`](spi2-contention-hil.md).
