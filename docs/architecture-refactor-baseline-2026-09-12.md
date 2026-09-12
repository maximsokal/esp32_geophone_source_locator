# Architecture refactor resource baseline

Captured for the ESP-IDF infrastructure migration on 2026-09-12. The
pre-refactor reference is the clean repository image at
`0777d74df532184b405a02b3e9fccdb89baec49f`; the post-refactor image is the
same production configuration after the migration. No flash, serial, or
physical-board action was performed.

## Measurement boundary

The software measurements were taken with ESP-IDF 5.5.4, Python 3.11.2,
CMake 3.30.2, Ninja 1.12.1 and Xtensa GCC `esp-14.2.0_20260121`. The target
build used `tools/run_idf.ps1` with `-Jobs 2` and the isolated
`build-refactor-final` directory. The size report came from the matching map
file with:

```powershell
python "$env:IDF_PATH/tools/idf.py" -B build-refactor-final size
python "$env:IDF_PATH/tools/idf.py" -B build-refactor-final size-components
python -m esp_idf_size --archives --format json build-refactor-final/esp32_geophone_source_locator.map
```

The baseline was captured before the first migration change. Runtime heap and
stack values require a running target log; `Get-CimInstance Win32_SerialPort`
reported no live serial ports (`SERIAL_PORTS=NONE`). Those fields are therefore
marked **NOT CAPTURED**, not estimated from a build.

## Before / after

| Measure | Before | After | Boundary |
| --- | ---: | ---: | --- |
| Production image file | `0x1fac80` (2,075,776 B) | `0x1ff640` (2,094,656 B) | COMPILES / flash image |
| Smallest app partition free | `0x205380` (51%) | `0x2009c0` (50%) | COMPILES |
| Bootloader | `0x5160`, 36% free | `0x5160`, 36% free | COMPILES |
| Flash code | 1,183,922 B | 1,197,018 B | `idf.py size` |
| Flash data | 767,412 B | 773,028 B | `idf.py size` |
| DIRAM used | 291,395 B (85.26%) | 292,291 B (85.53%) | `idf.py size` |
| DIRAM remaining | 50,365 B | 49,469 B | `idf.py size` |
| IRAM used | 16,384 B (100%) | 16,384 B (100%) | `idf.py size` |
| Map JSON total image size | 2,075,633 B | 2,094,513 B | `esp_idf_size --format json` |
| Internal heap free / largest | **NOT CAPTURED** | **NOT CAPTURED** | target UART required |
| DMA heap free / largest | **NOT CAPTURED** | **NOT CAPTURED** | target UART required |
| PSRAM heap free / largest | **NOT CAPTURED** | **NOT CAPTURED** | target UART required |

The image grew by 18,880 bytes on disk and DIRAM use grew by 896 bytes. This
is a migration accounting result, not an optimization claim. Official Button
and I2C-bus code is now part of the linked image; Insights and the LED backend
remain disabled in the baseline, and the DSP foundation has no active DSP
symbol use until measured filters are authorized.

## Task and stack contract

| Area | Before | After |
| --- | --- | --- |
| Application event delivery | UI event task, priority 6, stack 6,144 B | ESP-IDF `APP_EVENT` user loop, Core 0, priority 6, stack 6,144 B, queue 24 |
| Keypad input | App-owned keypad task, priority 4, stack 8,192 B | Five official ADC Button objects; no app-owned keypad task |
| Power input | App-owned power-button task, priority 5, stack 3,072 B | Official GPIO Button object plus EventGroup shutdown latch; no app-owned power task |
| App-owned input stack reservation | 11,264 B across two input tasks | Removed; vendor Button service overhead must be measured on hardware |
| UI event callback | Could wait for the LVGL lock | Bounded pending queue and wake; LVGL task/timer applies mutations |

The migration removes approximately 11,264 bytes of application task-stack
reservations and two application-owned input tasks. The official Button
component may create its own service task; its runtime HWM is intentionally not
claimed without a target log.

Requested runtime stack HWM fields for `sys_evt`, `web_lifecycle`, `ui_event`,
`keypad`, `power_button`, `acquisition` and `recorder` are **NOT CAPTURED** for
both images because no live COM4/COM6 target log was available. The post-image
runtime observability surface reports the remaining application tasks
(`acquisition`, `recorder` and UI); the Button service requires a separate
vendor-task HIL observation.

## Archive contribution snapshot

Values are `flash_total / ram_total` from the map archive report. These rows
make the architectural trade-off visible without treating a component-level
size change as proof of runtime safety.

| Archive | Before | After |
| --- | ---: | ---: |
| `libapp_event_bus.a` | 232 / 4 B | removed |
| `libapp_events.a` | absent | 292 / 4 B |
| `libespressif__button.a` | absent | 7,694 / 515 B |
| `libespressif__i2c_bus.a` | absent | 3,406 / 184 B |
| `libkeypad.a` | 1,087 / 21 B | 692 / 26 B |
| `libpower_control.a` | 1,258 / 12 B | 965 / 28 B |
| `libstatus_indicator.a` | absent | 303 / 7 B |
| `libsensor_bus.a` | absent | 238 / 8 B |
| `libinsights_bridge.a` | absent | 48 / 0 B |
| `libesp_event.a` | 3,468 / 4 B | included by the user loop and consumers |

## Verification boundary

The post-refactor build proves target compilation/linking and image sizing.
Host tests prove deterministic contracts. Neither proves ADC Button voltage
classification under noise, electrical GPIO21 behavior, GPIO18 pulse timing,
INA3221/TCA9548A wiring, TFT pixels, SD media, Wi-Fi transport, task HWM,
500-Hz timing, or safe power removal. Those remain TARGET/HIL or
PHYSICALLY VERIFIED results and are gated separately by the COM4/COM6 runner.

## Follow-up profile accounting (2026-09-12)

The historical before/after table above predates the ADC settling, Insights
startup/profile, and power-button timing follow-up. The current production
baseline rebuilt in `build-refactor-final` is `0x1ff750` (2,094,928 B) with
`0x2008b0` (50%) free in the app partition. Its `idf.py size` total is
2,094,809 B before binary padding. The opt-in Insights profile was built in
the isolated `build-refactor-insights` directory with its own `sdkconfig`;
it is `0x20ec50` (2,157,648 B) with `0x1f13b0` (49%) free, and its
unpadded `idf.py size` total is 2,157,525 B. `idf.py size-components` exited
0 for both builds.

The profile build confirms the enabled Insights branch and test auth provider
while heap/Wi-Fi metrics and diagnostic variables remain disabled. These are
compile/size facts only; no cloud upload, runtime diagnostic overhead, target
heap/HWM capture, or physical acceptance is claimed.
