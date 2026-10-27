# All production menu rendering tests

These tests build the managed **LVGL 9.6.0** on the native host and run the real
`ui_shell.c`, Home, Objects and its input handler, Storage, Wi-Fi and its setup
steps, System/Hardware, live Measurements, complete `ui_workflow.c` and
`ui_scan_preview.c`, the 3D renderer, boot/recovery, chart builders, models/core,
translations and `ui_font_montserrat_14`. No LVGL/widget functions are mocked
or extracted. The font's actual line height is 19 px.

The headless LVGL Test display renders **320×240 RGB565**, with the software
renderer, DPI and themes used by `sdkconfig`. It needs no SDL window or board.
The fixture replaces ESP-IDF/RTOS allocation, clock, hardware snapshots and
service/file I/O. Waveform, voxel, occupancy and sensor data are injected
explicitly; asynchronous file loading, sensor transport and physical LCD/SPI
output are outside this test. SSIDs, passwords and network URLs are test data;
reference QR images never include real access credentials.

## Run

Requirements: the project's managed LVGL sources (normally fetched by the
ESP-IDF build), Python/pytest, CMake, Ninja and a native MSVC/GCC/Clang compiler.
The runner reuses native compiler discovery from `run_host_tests.py`; Windows
CMake/Ninja can also be found under `C:/Espressif/tools`. MSVC builds start fresh,
avoiding stale dependencies from localized include output. GCC/Clang use Ninja's
ordinary dependency tracking.

```powershell
python -m pip install -r tests/ui_render/requirements.txt
python -m pytest tests/python/test_ui_render.py --basetemp .test-build/ui-render/check-01 -q
```

Use a new `--basetemp` directory for each run; pytest clears that directory.
Tests are also collected by the existing Python/local verification scope.
Native build logs are under `.test-build/ui-render/native`; images, `index.html`
and `frames.jsonl` are under the reported pytest artifact directory.

To render/compare screenshots into a stable review directory:

```powershell
python tools/run_ui_render_tests.py
```

That command checks PNG comparisons; run pytest for the geometry/style/input
assertions as well. `--render-only` writes candidates without changing references.

## Reference integrity and run identity

### Ranging result references

`golden/{en,uk}_ranging_result_{success,partial,failed,cable_exceeded}_page*.png`
contains dedicated result screens, including every pair page reached by RIGHT
and wrap back to the first page. The fixture uses all ten distinct pairs among
MAIN (0) and four remotes (1-4), with fixed distances starting at 1234 mm.
Partial success keeps a missing 0-4 pair; the cable-exceeded case preserves its
6001 mm measurement and warning; complete failure shows unknown distances.

These screenshots render the production LVGL screen, transition from RANGING
using the workflow core, and refresh through a simulated topology service.
They do not represent physical BLE/I2C measurements. Pytest checks the displayed
status, exact values and units, unknown values, available action, footer, and
presence/comparison of every reference page in both languages.

`{en,uk}_ranging_manual_{pair,value,rejected,saving}.png` and
`{en,uk}_ranging_manual_saved_page*.png` cover manual correction. The result
screen exposes **Enter distances** beside Retry/Confirm and Back. Select a
pair with UP/DOWN, press OK, change the selected digit with UP/DOWN and move
between digits with LEFT/RIGHT; OK saves and recalculates. LEFT from the first
digit returns to pair selection; LEFT again cancels. Distances are integer mm.
The harness uses real application key handlers to change 6001 to 1500 mm,
checks the queued payload and a rejected save, then displays a manually entered
pair after a simulated successful backend response. Backend integration tests
separately exercise the real geometry solver and per-remote cable checks.

```powershell
python -m pytest tests/python/test_ui_render.py -q --basetemp .test-build/ui-render/ranging-review-01
```

The existing `*_LAYOUT_READY_*`, `*_CALIBRATING_*`, `*_RANGING_*` and
`*_ranging_progress.png` references cover preparation, calibration and waiting.

Each run replaces its local reference copies from the authoritative `golden/`
directory. Reusing an output directory cannot retain a deleted source reference.
Old LVGL `_err.png` diagnostics are saved under `previous-errors/` before the
reference copies are replaced; current mismatch images still appear beside
the current reference. Actual/diff images are not cleared by failure handling.

`run.json`, linked from the gallery, records the comparison status/counts,
UTC start/end time, platform/Python, reference-set hash and the native build
identity. `build.json` beside the executable records HEAD, application/harness
source fingerprint, native configuration/lock hashes, actual font hash, LVGL
version/source hash, compiler command/binary hash and executable hash. Source
fingerprints cover the current file contents, including uncommitted C/header
changes; HEAD alone is not treated as proof of a clean source tree.

The runner requires a matching successful build identity for the executable.
Its build and render entry points invalidate the active report before work
starts. Compiler/render failure, timeout, missing frames or an empty capture
leaves `FAIL` metadata and a failure page, not the previous successful gallery.
Partial native output is retained in `render.log`. Normal CLI comparison still
exits nonzero on a missing or different reference.

The pixel capture metadata's `checks` describe native rendering/PNG comparison only. Geometry,
style, input and reachability assertions are executed by pytest; this file does
not claim those assertions or physical LCD/hardware checks passed.

Run the boundary checks and the real renderer together:

```powershell
python -m pytest tests/python/test_ui_render_runner.py tests/python/test_ui_render.py --basetemp .test-build/ui-render/check-03 -q
```

For actual screen-registry transitions, result ownership, repeated lifecycles
and reproducible application-key stress, include
`tests/python/test_ui_navigation_render.py` in that command. The separate
`ui_navigation_render` binary links the full registry with real renderers;
the pixel executable above keeps its original direct-render fixture.
`navigation-lvgl*/index.html` links the ten EN/UK diagnostic galleries, key
journals, seeds and identities. See [G details](../../docs/ui-regression.md#native-routing-lifecycle-and-seeded-application-keys-g).
There is no baseline update. Only service/RTOS boundaries are controlled; actual
debounce, RTOS timing and physical TFT/SPI remain hardware acceptance.

Use a new basetemp name. The real renderer test copies the reference source into
an isolated directory, runs a successful comparison, removes only that isolated
source reference and reruns into the same report directory. The second run must
report a missing reference. Unit tests also cover strict CLI failure, build and
render failure after success, timeout output, empty capture, executable identity
and protection against writing output over authoritative goldens.

## Coverage and references

The fixture renders 774 states, including all eight registered screens and
their nested modes, in EN and UK. The gallery has menu/language filters and a
filter for changed/missing references. Every captured menu variant has a PNG.

| Menu | Included states |
| --- | --- |
| Home | Every selection, missing required hardware, SD recovery/failure |
| Objects | Loading/empty/max-size lists, page edges, full-length names, every selection mode, delete scope available/unavailable, confirmation/cancel, operation notices |
| SD | Data unavailable/ready, both actions, erase confirmation, service busy |
| Wi-Fi | No saved network, every setup/CA step, saved settings with disconnected phone, station/verification QR, verification complete/incomplete, setup/forget confirmation, errors |
| System/Hardware | EN/UK/Hardware selections, all module states and pod severity colors |
| Live Measurements | All service states, all review actions, transport faults/unavailable service, scrolling to all five node rows and back |
| Workflow | All 19 stages, 0–4 remotes, every action and result/progress variant |
| 3D | Loading/no-scan/unavailable reasons, sensor estimates, top/side/ISO/free views, registered/unregistered voxels, rotation, occupancy, recording/stop confirmation/pending/busy/error |
| Boot/Recovery | Every boot stage, mount/write errors, technical-code/warning branches, format progress and success/failure |

The fixture also covers all 19 workflow stages, 0–4 remotes, every action
selection through the real UP/DOWN handler, LEFT-to-Home, and OK-to-initialize
with 0, 1 and 2 remotes under the strict recording-gate configuration. It also
covers material selection, calculation/ranging progress and spinner, a result waveform,
ready/warning/error/disconnected colors and time-driven long-text scrolling.

Assertions use `lv_obj_update_layout()` and actual LVGL bounds, text metrics,
fonts and style colors. They check parent/screen containment, sibling/body/control
overlap, 300×31 standard root-menu rows, parent-sized nested rows, the 146×31
remote grid and full-size action selection. The result screen shows one action
at a time to leave room for charts; UP/DOWN reaches all choices.

Live diagnostics use a vertical viewport rather than shrinking text. Its native
LVGL clipping/scroll properties are recorded. Off-viewport content is allowed
only under an explicitly clipped vertical scroll container; inner cards still
must contain their labels. Real UP/DOWN events must bring all five node rows
fully into view and return to the top while the status/footer remain fixed.
Tests also require a screenshot for every screen ID and every Objects/Wi-Fi/
Measurement/Preview mode enum, so an added mode cannot silently lose coverage.

The reference set contains 468 PNGs in `golden/`. The built-in
`lv_test_screenshot_compare()` uses exact pixels, with automatic reference
creation disabled. Missing references fail. Mismatches leave an actual PNG,
LVGL's `_err.png`, and a Python `_diff.png` in the artifact directory. Tests also
deliberately alter a reference pixel and remove a reference in an isolated
directory to prove that both failure paths work.

For an intentional UI change, first render candidates with `--render-only` and
inspect the images/layout failures. Obtain explicit approval of the intended
reference changes before updating the baseline. Only after that approval:

```powershell
python tools/run_ui_render_tests.py --update-goldens
python -m pytest tests/python/test_ui_render.py --basetemp .test-build/ui-render/check-02 -q
git diff --stat -- tests/ui_render/golden
```

Normal test runs never accept or rewrite tracked references. Simulator pixels
do not prove the TFT's physical colors/orientation, live input timing or remote
sensor initialization on hardware.
