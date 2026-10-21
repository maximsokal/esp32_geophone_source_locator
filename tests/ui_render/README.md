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
CMake/Ninja can also be found under `C:/Espressif/tools`. It does a fresh native
build, avoiding stale dependencies from localized MSVC include output.

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

For an intentional UI change, first inspect the candidate images and layout
failures. Then explicitly regenerate and review the PNG diff:

```powershell
python tools/run_ui_render_tests.py --update-goldens
python -m pytest tests/python/test_ui_render.py --basetemp .test-build/ui-render/check-02 -q
git diff --stat -- tests/ui_render/golden
```

Normal test runs never accept or rewrite tracked references. Simulator pixels
do not prove the TFT's physical colors/orientation, live input timing or remote
sensor initialization on hardware.
