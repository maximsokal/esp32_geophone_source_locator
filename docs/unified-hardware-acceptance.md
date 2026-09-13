# Unified offline hardware acceptance report

`tools/analyze_hardware_acceptance.py` combines the saved-log evidence for MAX-6, MAX-7, MAX-10 and MAX-11 into one report. It never opens serial ports, flashes firmware, resets hardware, or controls power.

The expected workflow remains unchanged: the ESP32 is flashed manually on COM4, passive logs are observed/captured separately on COM6, and the resulting text log is analyzed offline.

## First pass without a measured storage baseline

```powershell
python .\tools\analyze_hardware_acceptance.py .\path\to\uart.log
```

This can evaluate MAX-6, MAX-7 and MAX-11. MAX-10 is deliberately reported `BLOCKED` until a measured storage baseline profile is supplied.

## Full machine-observable pass with a measured baseline

```powershell
python .\tools\analyze_hardware_acceptance.py `
  .\path\to\uart.log `
  --storage-baseline .\docs\hil-baselines\storage-spi-<measured-profile>.json
```

To specifically require reproduction of priority inheritance for MAX-11:

```powershell
python .\tools\analyze_hardware_acceptance.py `
  .\path\to\uart.log `
  --storage-baseline .\docs\hil-baselines\storage-spi-<measured-profile>.json `
  --require-inherited
```

The MAX-11 path always requires at least one targeted `STORAGE_PRIORITY stage=pre-release owner=web_status_cache` sample. `--minimum-priority-samples N` can require a longer/more representative capture.

## Issue mapping

### MAX-6 — keypad/input reliability

Machine evidence requires all five key codes at every software stage:

`KEYPAD_RAW_TRANSITION -> KEYPAD_BUTTON_PRESS -> KEYPAD_EVENT_POST -> KEYPAD_EVENT_DISPATCH -> UI_KEY_RESULT`

Stale navigation, spontaneous safe shutdown or crash/failure markers are machine failures.

Still manual/physical:
- visible TFT response;
- intentional post-startup GPIO21 long-press safe-shutdown behavior.

### MAX-7 — startup/storage responsiveness

Uses the full boot analyzer, including:
- `startup_total <= 5000 ms`;
- per-object recovery window `<= 250 ms`;
- recovery starts after interactive startup;
- Web cache completes after recovery stops;
- complete keypad chain/no stale navigation;
- no panic/watchdog/brownout/assert.

Still manual/physical:
- visually confirm TFT remains responsive while background storage work runs.

### MAX-10 — storage/recorder numerical HIL

Without `--storage-baseline`, MAX-10 is always `BLOCKED`.

With a valid measured profile, the report checks the real log against the seven profile limits plus recorder integrity, zero dropped frames, normal stop completion, AD7606 diagnostic coverage and acquisition/crash markers.

Still manual/provenance:
- confirm the baseline profile came from the actual card/board/configuration and not synthetic parser fixtures.

### MAX-11 — `web_status_cache` priority inheritance

Requires targeted pre-release storage-owner telemetry, which is sampled while the storage recursive mutex is still held.

The analyzer checks:
- base priority remains 1;
- `inherited` agrees with `current > base`;
- optional `--require-inherited` requires an actual boosted sample.

If closing by “the historical boost no longer reproduces”, document the stress workload/capture duration and use an appropriate `--minimum-priority-samples`; one base-priority sample alone is not proof.

## Result semantics

`HARDWARE_ACCEPTANCE_MACHINE_RESULT` is only the aggregate **machine-observable** result:

- `PASS` — every included machine check passed;
- `BLOCKED` — evidence is incomplete (for example no measured MAX-10 baseline, missing key, or insufficient priority samples);
- `FAIL` — captured evidence proves at least one violation.

The report prints `MANUAL_REQUIRED:` lines explicitly. A machine PASS must not be used to erase those physical/manual acceptance requirements.
