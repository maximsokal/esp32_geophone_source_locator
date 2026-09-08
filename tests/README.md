# Firmware test foundation

The repository has three intentionally separate test surfaces:

* `tools/run_host_tests.py` builds and runs the 21 C contract suites in
  `tests/host/manifest.json`. It applies strict C11 warnings consistently and
  writes a machine-readable report under `build/host-tests`.
* `tests/python` contains deterministic pytest checks. Run `python -m pytest`
  from the repository root. HIL tests are marked `hil` and are skipped by
  default.
* `test_app` is a standalone ESP-IDF 5.5.4 Unity application. It links four
  deterministic production-policy cases without changing the production
  firmware app. The target app runs all four cases automatically and prints a
  `TEST_APP_RESULT` marker; the board still requires an explicit HIL session.

The WebUI console contract in `webui/tests/console/deviceApiSerialization.test.ts`
validates the four-channel `/api/seismic` shape before rendering and carries a
signed impulse through `fetchSeismic()` into the waveform polyline helper used
by `LiveSeismicPanel`. This is deterministic data-path evidence; live Wi-Fi
transport, browser layout and physical pixel output still require a connected
board and browser HIL session.

## Host tests

```powershell
python tools/run_host_tests.py --list
python tools/run_host_tests.py
python tools/run_host_tests.py --compiler .tools/llvm-mingw/llvm-mingw-20260826-ucrt-x86_64/bin/clang.exe --sanitizers address,undefined
python tools/run_host_tests.py --compiler .tools/llvm-mingw/llvm-mingw-20260826-ucrt-x86_64/bin/clang.exe --coverage
```

With no `--compiler`, the runner discovers the installed native compiler (the
current Windows environment uses MSVC `cl.exe`). Sanitizer and coverage
requests fail explicitly if the selected native compiler does not support them.
The Espressif compiler shipped with ESP-IDF is a target cross compiler and is
rejected as a substitute for a native host compiler. Set `HOST_CC` or pass
`--compiler` when several native compilers are installed.

## HIL gate

HIL tests require both explicit command-line opt-in and an environment marker:

```powershell
$env:GEOLOCATOR_HIL = "1"
python -m pytest --hil -m hil
```

Any fixture that can flash, monitor, or communicate with physical hardware must
depend on the `hil_enabled` fixture. The default `python -m pytest` command
cannot enter that fixture and performs no hardware operation.

The Unity test app is built separately with the ESP-IDF environment selected by
the caller:

```powershell
Set-Location test_app
python "$env:IDF_PATH/tools/idf.py" -B build-s3 set-target esp32s3
python "$env:IDF_PATH/tools/idf.py" -B build-s3 build
```

## VS Code tasks

The local `.vscode/tasks.json` file keeps the verification surfaces separate:

* `Tests: All (no flash)` runs host C, pytest, WebUI tests/typecheck, the Unity
  target build, and the production build. It never writes hardware.
* `Tests: Web` runs the console, WebXR, and typecheck tasks independently of
  firmware.
* `ESP: Unity tests (flash COM4 + logs COM6)` starts the passive COM6 monitor,
  flashes the Unity app through COM4, and requires `TEST_APP_RESULT: PASS`.
* `ESP: Flash production (COM4)` and `ESP: Monitor logs (COM6)` are separate
  production-firmware tasks. `ESP: Check serial ports` validates live ports
  before a combined HIL task; the flash and monitor tasks use their matching
  `ESP: Check COM4` or `ESP: Check COM6` preflight.
* `ESP: Production pipeline HIL (COM4 + COM6)` is a separate explicit task. It
  builds production first, then runs `run_pipeline_hil.ps1 -RequireFullPipeline`
  with COM4/COM6. VS Code prompts for the HTTPS base URL and an existing object
  ID before the explicit flash/recording run; it is intentionally absent from
  `Tests: All (no flash)`.

The `Tests: Web` and `Tests: All (no flash)` tasks call
`tools/run_suite.ps1`. The runner keeps each command's normal output, then
prints a table with one `PASS`, `FAIL`, or `BLOCKED` row per step and a final
`SUITE_RESULT: PASS|FAIL|BLOCKED` line. A zero exit code means every requested
local step passed. In the `All` report, `ESP Unity HIL (COM4+COM6)` is shown as
`SKIPPED`: the suite only builds ESP images and never proves board tests. Run
`ESP: Unity tests (flash COM4 + logs COM6)` for the separate hardware result;
that task must produce `TEST_APP_RESULT: PASS` from the COM6 log.

The PowerShell helpers under `tools/` pin ESP-IDF 5.5.4 to the Python 3.11.2
environment used by the existing build cache. They do not erase flash or alter
the SD card.

The target build proves compilation and linking only. No flash, serial monitor,
or physical-board result is implied by this command.

## Production pipeline HIL runner

`tools/run_pipeline_hil.ps1` is an explicit, production-firmware HIL command;
it is never called by the no-flash suite. A live run requires both live COM4
and COM6 ports, starts the passive COM6 monitor, and flashes production on
COM4 unless `-SkipFlash` is supplied:

```powershell
powershell -File tools/run_pipeline_hil.ps1
powershell -File tools/run_pipeline_hil.ps1 -SkipFlash -BaseUrl https://geolocator.local
powershell -File tools/run_pipeline_hil.ps1 -BaseUrl https://geolocator.local -Record -ObjectId OBJ_0001
powershell -File tools/run_pipeline_hil.ps1 -RequireFullPipeline -BaseUrl https://geolocator.local -Record -ObjectId OBJ_0001
powershell -File tools/run_pipeline_hil.ps1 -RequireFullPipeline -BaseUrl https://geolocator.local -Record -ObjectId OBJ_0001 `
  -ConfirmTftRendering -ConfirmKeypadResponsive -ConfirmPowerButtonResponsive
```

For parser-only replay, pass a captured combined serial log. Replay mode does
not inspect serial ports, start a monitor or flash anything:

```powershell
powershell -File tools/run_pipeline_hil.ps1 -LogPath .\build\hil\pipeline-production.log
```

The final report checks the AD7606 GPIO/SPI/DMA/self-test/start markers (including
the configured `-ExpectedSampleRateHz`), all eight AD7606 channel diagnostics,
panic/reset/read-failure markers and, when `-BaseUrl` is provided, `/api/seismic`
metadata, exact channel IDs `[1,2,3,4]`, bounded per-frame timestamp cadence and
`/api/status`.
The production firmware exposes `/api/status.runtime.now_ms` and
`/api/status.runtime.tasks` for acquisition, recorder, UI, keypad and
power-button loops. The runner compares two status snapshots, requires every
heartbeat to advance, and requires each `last_heartbeat_ms` to be no older than
`-MaxTaskHeartbeatAgeMs` (default 1000 ms) at the device snapshot time; a
recording run also requires acquisition and recorder progress to advance. The
recorder heartbeat is emitted after its buffer copy, SD write and any flush
return, so a blocked storage call cannot create a false liveness report. The UI
beat follows event receive and LVGL dispatch, while keypad, power-button and
acquisition beats follow their poll/read delays and buffer publication. This
catches a stalled FreeRTOS task automatically, while it still cannot prove TFT
pixels or electrical button response.
`-Record -ObjectId` additionally exercises explicit recording start/stop and
requires the completed session metadata returned by `/api/status?sessions=...`
to contain the exact newly-created session ID, expected rate, four channels, a
nonzero persisted sample count and `dropped_frames=0`. The status acquisition
object must expose `read_errors` and `consecutive_failures`, both zero, for a
healthy HIL pass.
`-RequireFullPipeline` turns omitted HTTP/SD checks and the unautomatable
physical TFT/keypad/power-button checks into `BLOCKED`, preventing a partial
serial/API run from being reported as a complete pipeline result. The
`-Confirm...` switches are valid only with `-RequireFullPipeline`; this keeps a
partial command from being reported as a full pass. After the operator observes
the live Measurements screen, keypad navigation and ordered power-button
shutdown during acquisition, the three explicit
`-ConfirmTftRendering`, `-ConfirmKeypadResponsive` and
`-ConfirmPowerButtonResponsive` switches record those manual attestations as
`PASS`. The switches are rejected in `-LogPath` replay because a captured log
cannot prove physical behavior.
`PIPELINE_HIL_RESULT: PASS` means the executed checks passed. TFT pixels and
physical keypad/power-button responsiveness are reported as limitations and
still require a human or camera-assisted board run while acquisition is live.
The API client uses normal certificate validation; for the project's local
self-signed HTTPS certificate, install/trust the project CA in the invoking
PowerShell account before using an `https://` `-BaseUrl`.

For a live Measurements-screen run, keep the passive COM6 adapter receive-only
and power the board from its normal supply. Open the Measurements screen before
starting acquisition, then exercise LEFT/UP/DOWN/RIGHT/OK while the runner is
polling and confirm that navigation and the waveform remain responsive. Press
the power button once during the run and confirm that acquisition stops before
the ordered shutdown completes. Record those TFT/keypad/power observations next
to the runner's terminal report; serial and API evidence cannot observe pixels
or button latency by themselves.

The `seismic_pipeline_scheduling_contract` host suite feeds deterministic
AD7606-labelled frames through the production seismic ring buffer while
servicing recorder batches, UI preview reduction and web snapshots at
different rates. It checks independent consumers, preserved signed impulses,
sample timestamps/rate, bounded ring overflow reporting and the configured
Core 0/Core 1 task-priority separation. It is a boundary contract: it cannot
prove SPI DMA timing, SD media writes, TFT pixels, Wi-Fi transport or physical
keypad/power-button responsiveness; those require the target application and
an attached board.

The `seismic_recorder_web_pipeline_contract` suite drives the same published
AD7606-labelled frames through the production recorder filesystem helper and
the `/api/seismic` production handler. It verifies the `GEOSEIS1` binary
header, frame bytes, sample-rate/timestamp metadata, and signed impulse values
in the captured chunked JSON response. Narrow host fakes stand in for the SD
mount and HTTP server; physical media, Wi-Fi, SPI DMA and task scheduling still
require HIL evidence.
