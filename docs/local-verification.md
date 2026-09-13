# Local verification workflow

Normal project verification is local. GitHub Actions are manual-only and are not part of the routine push/verification path.

## Deterministic software verification

From the repository root:

```powershell
python tools/run_local_verify.py --scope software
```

This runs, in order:

1. `tests/host/manifest.json` through `tools/run_host_tests.py`;
2. every `tests/host/manifest.d/*.json` fragment in deterministic name order;
3. the complete `tests/python` suite without `--hil` (physical HIL is therefore disabled by `tests/python/conftest.py`);
4. WebUI `npm ci`, typecheck, console tests, XR tests, production build, and bundle verification.

Use `--skip-npm-ci` only when intentionally reusing the current `webui/node_modules` tree.

## Local ESP-IDF firmware build

On the project Windows workstation:

```powershell
python tools/run_local_verify.py --scope firmware --jobs 2
```

The runner invokes the repository's existing `tools/run_idf.ps1 -Action build`, which uses the locally installed ESP-IDF 5.5.4 toolchain. It does not flash the board and does not open a serial monitor.

On non-Windows systems the firmware scope requires `idf.py` on `PATH`. If it is unavailable, verification fails explicitly; it never falls back to GitHub Actions.

To run software plus firmware locally:

```powershell
python tools/run_local_verify.py --scope all --jobs 2
```

## Narrow scopes

For faster investigation:

```powershell
python tools/run_local_verify.py --scope host
python tools/run_local_verify.py --scope python
python tools/run_local_verify.py --scope web
```

`--dry-run` prints the planned local commands without executing them.

## Evidence boundary

A local software pass proves only the host/source/parser/WebUI behavior those tests exercise. It does not prove physical keypad behavior, TFT pixels, SPI2/SD electrical timing, DMA behavior, real FreeRTOS scheduling, or ESP32 runtime performance. Those remain LIVE BOARD HIL/manual evidence as defined in `docs/verification-evidence-matrix.md`.
