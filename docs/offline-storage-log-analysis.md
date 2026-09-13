# Offline storage/SPI/recorder log analysis

`tools/analyze_storage_spi.py` analyzes an **already captured** UART log. It does not open COM ports, flash firmware, reset hardware, or control power.

The analyzer is a Python equivalent of the storage/SPI telemetry contract and is intended for deterministic local/replay analysis without GitHub Actions or PowerShell.

## Thresholds are intentionally mandatory

There are no built-in performance limits. The seven latency thresholds must come from a measured real-board baseline for the actual SD card, 20 MHz SDSPI configuration, sample rate and enabled channel count.

Example shape only:

```powershell
python .\tools\analyze_storage_spi.py .\path\to\uart.log `
  --max-mutex-wait-us <MEASURED_LIMIT> `
  --max-tft-quiesce-us <MEASURED_LIMIT> `
  --max-tft-resume-us <MEASURED_LIMIT> `
  --max-spi-end-elapsed-us <MEASURED_LIMIT> `
  --max-recorder-window-us <MEASURED_LIMIT> `
  --max-recorder-fwrite-us <MEASURED_LIMIT> `
  --max-recorder-operation-us <MEASURED_LIMIT>
```

Do not copy values from parser unit tests into a hardware baseline. Those values are synthetic boundary fixtures only.

Optional JSON output:

```powershell
python .\tools\analyze_storage_spi.py .\path\to\uart.log <threshold arguments> --json
```

Exit codes:

- `0` — telemetry `PASS` for the supplied limits;
- `1` — `FAIL`, because captured evidence violates a threshold or integrity/health rule;
- `2` — `BLOCKED`, because the log lacks enough evidence for a decision.

## Required telemetry

By default the analyzer requires:

- at least one `STORAGE_SPI_TIMING stage=begin` sample;
- at least one `STORAGE_SPI_TIMING stage=end` sample;
- at least one `RECORDER_IO_TIMING op=fwrite` sample;
- at least one recorder `STOP_COMPLETE` sample;
- positive `AD7606_DIAG` samples from all eight channels.

Minimum sample counts are configurable, but reducing them changes the evidence requirement and must be justified for the test configuration.

## Integrity and acquisition checks

A latency result cannot pass if data integrity or acquisition health is bad. The analyzer rejects:

- non-zero storage begin result;
- non-zero recorder fwrite result;
- short writes (`written != requested`);
- recorder writes reporting zero storage windows;
- stop stage other than `complete`;
- any dropped recorder frames;
- stop error other than `ESP_OK`;
- AD7606 acquisition read failures, seismic-buffer push failures, or acquisition initialization failures;
- panic/watchdog/brownout/heap-corruption/abort/assert markers.

## Numerical checks

The analyzer reports and thresholds the maxima of:

- `mutex_wait_us`;
- `tft_quiesce_us`;
- `tft_resume_us`;
- storage `elapsed_us`;
- recorder `max_window_us`;
- recorder `fwrite_us`;
- recorder `operation_us`.

Exact equality with a supplied limit passes; only values above the limit fail.

## Evidence boundary

A Python/replay PASS validates parser logic against a captured log. It does **not** prove physical SD, SPI2, TFT DMA, FreeRTOS scheduling, AD7606 acquisition or dropped-frame behavior unless the input log was captured from the real device under the documented hardware configuration.

MAX-10 remains open until real-board baseline limits are measured, recorded, and a live capture passes those limits with healthy acquisition and zero drops.
