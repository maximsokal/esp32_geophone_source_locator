# Local verification workflow

Normal project verification is local. GitHub Actions are manual-only and are not part of the routine push/verification path. This file is the master project-wide `Run All` validation instruction; the detailed nRF54L15 work package is [`nrf54l15-test-task.md`](nrf54l15-test-task.md).

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

This is the existing deterministic host/WebUI software stage. It does not
flash or monitor hardware.

Use `--skip-npm-ci` only when intentionally reusing the current `webui/node_modules` tree.

## Local ESP-IDF firmware build

On the project Windows workstation:

```powershell
python tools/run_local_verify.py --scope firmware --jobs 2
```

The runner invokes the repository's existing `tools/run_idf.ps1 -Action build`, which uses the locally installed ESP-IDF 5.5.4 toolchain. It does not flash the board and does not open a serial monitor.

On non-Windows systems the firmware scope requires `idf.py` on `PATH`. If it is unavailable, verification fails explicitly; it never falls back to GitHub Actions.

## Isolated nRF54L15 build and test boundary

The nRF54L15 project is not part of the ESP-IDF root build. From the repository
root, use the wrapper because the checkout path contains spaces:

```powershell
.\nRF54L15\pio.ps1 build
```

The supported maintenance actions are `.\nRF54L15\pio.ps1 clean`,
`.\nRF54L15\pio.ps1 upload`, `.\nRF54L15\pio.ps1 monitor` and
`.\nRF54L15\pio.ps1 test`. Do not make
`pio run -d ".\nRF54L15"` the normal command. Generated PlatformIO/Zephyr output
belongs under `%USERPROFILE%\.platformio-workspaces\nrf54l15`, outside the
repository.

The wrapper build and the no-upload test launch are executable boundaries. The
current `nRF54L15/test` directory has no `test_*` suites, so `pio.ps1 test`
prints `nRF54L15 TEST_RESULT: SKIPPED` and does not claim nRF unit/native
evidence. Once suites exist, the action runs PlatformIO through the same
SUBST/workspace workaround with `--without-uploading`.

For the isolated local verifier, use:

```powershell
python tools/run_local_verify.py --scope nrf
```

This plans `nrf/build` followed by `nrf/tests`; `--scope all` includes the same
two steps alongside the ESP32-S3 and WebUI surfaces.
The default `--scope software` remains the existing host/Python/WebUI-only
software scope; use `nrf` or `all` when the nRF surface is required.

## Master project-wide validation (`Run All`)

The existing no-flash entry points remain:

```powershell
python tools/run_local_verify.py --scope all --jobs 2
```

and VS Code `Tests: All (no flash)`, which calls `tools/run_suite.ps1 -Suite All`.
The master validation contract is now staged as follows:

1. **ESP32-S3:** configure/build, native/component tests, Python source and
   replay/parser contracts, and the existing production/Web asset checks.
2. **nRF54L15:** isolated wrapper build and the nRF PlatformIO test launch
   through `pio.ps1 test`. The current empty test surface is explicitly
   `SKIPPED`; future native/unit suites run from this same entry point. This
   stage uses one firmware/environment for every node and never creates
   central/remote builds.
3. **WebUI:** existing install, typecheck, console/XR tests, build and bundle
   checks.
4. **Integration:** ESP32/nRF protocol/schema contracts when they exist,
   architecture/task consistency, and hardware pin/configuration consistency.
5. **Physical tiers:** run only as explicitly authorized HIL tasks after the
   software stages; they are not hidden inside the no-flash `All` result.

`Tests: All (no flash)` calls `tools/run_suite.ps1 -Suite All`, which now runs
the nRF wrapper build and records the nRF test stage in the same report. The
standalone `run_local_verify.py --scope all` plan likewise contains
`nrf/build` and `nrf/tests`. Neither path flashes nRF hardware; physical
one-board, multi-node and full-topology tiers remain explicit HIL work.
For a connected board, the VS Code task `nRF54L15: Flash firmware` invokes
`nRF54L15/pio.ps1 -Action upload` through the same path-safe wrapper. A
successful upload only proves programming completed; it is not by itself a
single-board or multi-node HIL result.

### Validation tiers and blocking policy

| Tier | Runs automatically on a developer PC/CI | Physical prerequisite |
| --- | --- | --- |
| Software-only | ESP32 host/Python/WebUI checks, ESP32 builds, nRF wrapper build and no-upload test launch, source/schema/docs contracts | None; no physical nRF board; an empty nRF test surface is explicit `SKIPPED` |
| ESP32 target | ESP32 Unity/production live runners and serial/API evidence | One physical ESP32-S3 and its required ports/wiring |
| One nRF54L15 | boot, USB log, single-board target checks, remote LED/IMU/I2C checks | One physical XIAO nRF54L15; Sense variant where IMU is required |
| Multiple nRF54L15 | pairwise ranging, matrix, role rotation, reconnect/missing-node tests | Two or more physical nRF54L15 boards |
| Full topology | ESP32 upstream I2C + TCA9548A channels + main nRF + remote nodes + independent BME280 power | Complete wired diploma topology |

Absence of five physical nRF54L15 boards must never block ordinary
software-only CI. Missing hardware or telemetry in a physical tier is
`BLOCKED`, not an implicit pass. Replay/native/simulator results remain
software evidence and do not become physical HIL proof.

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
