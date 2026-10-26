# ESP32 Geophone Source Locator

ESP32-S3 diploma prototype for seismic search under rubble. The firmware acquires multiple geophone channels simultaneously, stores object/measurement data on microSD, exposes a local web interface, provides a local TFT UI, and now contains a reusable 3D TDOA geometry solver for the later localization pipeline.

The project targets **ESP-IDF v5.5.4** on an ESP32-S3 N16R8 board.

For the component/runtime design, task ownership and extension rules, see [ARCHITECTURE.md](ARCHITECTURE.md).

Structural code search and linting use [ast-grep](docs/ast-grep.md).

## Current hardware architecture

### ESP32-S3

The controller is an ESP32-S3 with 16 MB flash and 8 MB octal PSRAM.

PSRAM runs at 80 MHz. If the board is unstable (random crashes after a ~20 °C temperature change or boot failures), revert to `CONFIG_SPIRAM_SPEED_40M`; HIL validation is required.

GPIO33-GPIO37 are occupied by octal PSRAM internally and are intentionally not assigned to application peripherals.

### SPI2: UI and storage

SPI2 is shared only by the TFT and microSD card:

| Signal | ESP32-S3 GPIO |
| --- | ---: |
| MOSI | 11 |
| SCLK | 12 |
| MISO | 13 |
| TFT CS | 14 |
| TFT DC | 15 |
| TFT RESET | 16 |
| microSD CS | 17 |

The current firmware sets the microSD SPI runtime ceiling to **20 MHz** (`SDMMC_FREQ_DEFAULT`); discovery starts at 400 kHz. This is the configured ceiling, not a claim that the present wiring has been physically validated at that rate.

### SPI3: dedicated AD7606 acquisition bus

AD7606 is isolated from the display/storage bus and owns SPI3:

| AD7606 signal | ESP32-S3 GPIO |
| --- | ---: |
| DOUTA / DB7 / MISO | 39 |
| RD / SCLK | 40 |
| CS | 41 |
| CONVST A + CONVST B | 42 |
| BUSY | 47 |
| RST | 2 |

AD7606 serial mode is read-only, therefore SPI3 MOSI is unused. A 10 kOhm pull-up is used on AD7606 CS.

Current AD7606 board configuration:

- RANGE low: +/-5 V input range;
- OS0/OS1/OS2 low: no hardware oversampling;
- 8 simultaneous channels;
- SPI mode 2;
- 1 MHz SPI clock;
- default acquisition rate: 500 Hz.

The first four AD7606 channels currently feed the application seismic ring buffer. All eight channels are read on every conversion and are available to the local TFT diagnostics UI.

### Other current GPIO assignments

| Function | GPIO |
| --- | ---: |
| Analog keypad KS0466 | 8 |
| Pololu power-off control | 18 |
| External power-button sense | 21 |

GPIO1 is reserved by the existing divider/bench wiring and must not be reused for AD7606.

### nRF54L15 subproject

`nRF54L15/` is an independent PlatformIO + Zephyr project for all XIAO
nRF54L15 nodes. It has one production source tree, one production environment
and one firmware binary; runtime topology and Bluetooth initiator/reflector
roles do not create central/remote builds. The main node is on the ESP32-S3
upstream I2C bus; remote nodes are behind TCA9548A channels. Each remote nRF
locally aggregates its GY-BMI160 and BME280 on the second XIAO I2C pair
(`D11/P0.03` SCL, `D12/P0.04` SDA), powered from XIAO `3V3_OUT`. These local
sensors are not direct ESP32/TCA devices. RJ45 pin 2 (+3.3 V) is reserved/NC
and must not be tied to `3V3_OUT`.

The installed boards are the **plain XIAO nRF54L15, not the Sense variant**,
so they have no built-in IMU. Each remote geophone uses an external
**GY-BMI160** and BME280 on the XIAO's private second I2C bus: `D11/P0.03` is
SCL and `D12/P0.04` is SDA. The upstream ESP32/TCA bus remains on
`D4/P1.10` SDA and `D5/P1.11` SCL. The main-enclosure nRF54L15 does not
require the remote-only sensor assembly.

The REMOTE logger LEDs show GREEN while READY when no fault has latched; GREEN
means “no latched fault,” not that sensor preparation has completed. An
unattempted sensor is pending, while a failed BMI160 or required BME280 init is
an immediate ERROR. Runtime IMU/ENV fault bits count only after that sensor is
READY. WARNING must persist for 2000 ms before YELLOW latches; ERROR/FATAL is
immediate, and latches clear only on reset. `LOG_RING_LOSS` latches WARNING
when `overwritten_total > 0` (internal logger losses also map to WARNING). The
128-record ring can overwrite during a bench run without an ESP32 collector,
producing a latched YELLOW; this is expected bench behavior.

The authoritative topology, local sensor/power contract, external
GY-BMI160/verticality contract, Channel Sounding role rotation and remote LED
pinout are in
[`ARCHITECTURE.md`](ARCHITECTURE.md). The future nRF implementation and
testing task is [`docs/nrf54l15-test-task.md`](docs/nrf54l15-test-task.md).

## UI architecture

The TFT UI uses **LVGL 9** through Espressif's official **esp_lvgl_port** integration. The display component is limited to the physical `esp_lcd` / ILI9341 layer; widgets, text, QR rendering, navigation, refresh timing and buffering belong to LVGL.

Managed dependencies are pinned through the ESP-IDF Component Manager:

- `espressif/esp_lcd_ili9341 ^2.0.2`;
- `espressif/esp_lvgl_port ^2.9.0`;
- `lvgl/lvgl ^9.5.0`;
- `espressif/button 4.2.0` for the keypad and power-button adapters;
- `espressif/i2c_bus 1.5.2` for the shared INA3221 sensor bus;
- `espressif/led_indicator 2.1.2` as the disabled, board-wiring-gated status backend;
- `espressif/esp-dsp 1.8.2` for the task-free measured-signal foundation;
- optional `espressif/esp_insights 1.3.4`, disabled in the baseline image.

The previous project-local 5x7 bitmap font and custom text-rendering stack are no longer part of the active display architecture.

The LVGL handler task is pinned to **Core 0**. The five-button analog keypad
uses official ADC Button objects over the existing ADC1 owner, and the GPIO21
power input uses the official GPIO Button object with external pull circuitry.
Both adapters publish to the dedicated ESP-IDF `APP_EVENT` user loop; the UI
subscriber only queues events and applies them from the existing LVGL timer.
The event loop contract is documented in
[`ARCHITECTURE.md`](ARCHITECTURE.md), and the before/after resource snapshot is
in [`docs/architecture-refactor-baseline-2026-09-12.md`](docs/architecture-refactor-baseline-2026-09-12.md).

Screen behavior is registered through one `ui_screen_driver_t` table. Open/render/key/refresh behavior and the Home menu label therefore have one source of truth instead of duplicated enum/switch routing.

Current screens:

- Home;
- Objects;
- Measurements;
- Storage;
- Wi-Fi / Web;
- System.

The **Measurements** screen is live, not a placeholder. For the AD7606 build it displays:

- acquisition source and RUN/STOP state;
- sample rate and channel count;
- real-time core assignment;
- SPI3 transport, SPI mode and bus clock;
- configured +/-5 V range and hardware oversampling ratio;
- successful sample count, read errors and consecutive failures;
- age of the last simultaneous sample;
- live V1..V8 values in mV.

The UI consumes a read-only `seismic_acquisition_snapshot_t`; it does not access AD7606 GPIO or SPI functions directly.

## CPU/task policy

Application-owned tasks are split by role.

### Core 0 - services

- LVGL handler and bounded UI event application;
- ESP-IDF `APP_EVENT` user loop and official Button service;
- object service;
- storage service;
- Wi-Fi/HTTP-facing application work.

### Core 1 - real-time side

- AD7606 acquisition;
- synthetic seismic test source;
- future signal preprocessing / event detection;
- future continuous localization worker if required.

All application task priorities, stacks and core assignments are centralized in `components/app_config/include/app_task_config.h`.

The AD7606 task initializes and owns SPI3 on Core 1 so the dedicated acquisition bus and its interrupt affinity remain on the real-time side.

## Seismic acquisition pipeline

For the AD7606 source:

```text
geophone
   -> low-noise analog amplifier/filter
   -> AD7606 simultaneous analog inputs
   -> CONVST
   -> timestamp captured at the sampling event
   -> BUSY completion
   -> SPI3 read of V1..V8
   -> acquisition diagnostics snapshot
   -> seismic ring buffer
   -> web / recording / future signal processing
```

The frame timestamp is captured immediately after the CONVST rising edge, not after BUSY handling and SPI transfer.

The seismic ring buffer uses short metadata critical sections plus per-slot publication sequencing. HTTP snapshots do not hold a long mutex while copying hundreds of waveform frames, so a slow web reader does not block the acquisition producer for the duration of the copy.

## Acquisition source interface

The source is selected at build time in `menuconfig`:

- `AD7606 SPI acquisition` - normal hardware path;
- `Synthetic test waveform` - software-only waveform source.

Each backend implements the same private `seismic_source_driver_t` interface. The acquisition orchestrator itself is backend-agnostic; compile-time selection is isolated in the source registry. Runtime source switching is intentionally not exposed yet.

Current backend files:

- `seismic_source_ad7606.c`;
- `seismic_source_test.c`.

Adding a new source should add a descriptor and registry entry rather than adding source-specific branches to `seismic_acquisition.c`.

## TDOA localization component

The repository now contains an independent `localization` component with a reusable 3D TDOA solver:

```c
esp_err_t localization_tdoa_solve(...);
```

It accepts:

- 4..8 sensor positions in meters;
- one arrival timestamp per sensor for the same detected event;
- a propagation speed in m/s;
- an optional initial source-position guess.

It returns:

- estimated `(x, y, z)` source position;
- estimated emission time;
- RMS timing residual;
- iteration count;
- convergence status.

The solver rejects degenerate/coplanar 3D sensor geometry and uses damped nonlinear least-squares iterations.

Beside it, `localization_velocity_free` provides three methods that estimate the event's own velocity: **TDCAS** (P picks, 6 sensors or 5 on a flat array), **PSAFUVS** (P and S picks, 4 flat sensors suffice) and **SUEA** (shortest travel paths around empty areas). "Calculate source" runs all of them; when the session has a registered 3D scan, their scan-constrained variants use the scan's empty (FREE) and filled (OCCUPIED) space. See [ARCHITECTURE.md](ARCHITECTURE.md#velocity-free-methods-and-the-3d-scans-emptyfilled-space).

`localization_extended` adds five more: **cross-correlation TDOA** over every pod pair, a **robust** outlier-rejecting fit, **PSP** (P-TDOA and S-P jointly), **eikonal** travel times that go around the scan's voids (plus tomography from calibration hits), and a **Bayesian** posterior that reports a 90% credible region instead of a bare point. With four flat pods, place them off a common circle (not in a rectangle) if you want the velocity-free methods. See [ARCHITECTURE.md](ARCHITECTURE.md#extended-methods-waveform-delays-robust-fits-p-s-eikonal-posterior).

**Important:** this does not mean the end-to-end locator is finished. The firmware still needs a validated preprocessing/event-detection stage that converts raw geophone data into reliable per-channel arrival timestamps. The propagation speed also cannot be hard-coded as a universal constant for heterogeneous rubble; `localization_default_config()` deliberately leaves `propagation_speed_mps` unset so the caller must provide a measured or modelled value.

Planned end-to-end path:

```text
AD7606 frames
   -> filtering / preprocessing
   -> event detection
   -> per-channel arrival-time picking
   -> sensor position / height data
   -> propagation model
   -> localization_tdoa_solve()
   -> source position + residual / quality
```

## Current timing limitation

Timestamp placement is already correct: timestamps represent CONVST, not later SPI completion.

The periodic trigger is still initiated by `esp_timer` + task notification. That is acceptable for the current bench stage, but it is not claimed to be the final deterministic sampling architecture. If the later TDOA error budget shows scheduler/trigger jitter to be significant, CONVST timing must move to a hardware-timed path rather than merely changing where software reads a timestamp.

## Storage and safe shutdown

Object and measurement state is stored under `/sdcard/geolocator`.

The shutdown sequence stops producers/services before cutting power:

```text
official Button adapters / shutdown latch
-> INA3221 monitor
-> shared sensor_bus / I2C0 release
-> measurement service / recorder finalization (LVGL guard still alive)
-> APP_EVENT UI subscriber / LVGL port
-> storage service
-> object service
-> seismic acquisition
-> HTTP / Wi-Fi
-> storage manager
-> SD unmount
-> display disable/deinit
-> power-off pulse
```

The SD handle is cleared only after successful FAT unmount. If unmount fails, software state still reflects that the card handle is present.

The TFT Storage screen requires a second confirmation before application-data erase is started.

## Web interface

The firmware starts a local HTTPS server on port 443 (`https://geolocator.local/`) and exposes device/seismic APIs including:

- `/api/status`
- `/api/seismic`

`/api/seismic` can return recent waveform frames without holding the acquisition producer behind a long reader-side copy.

The web component exposes only `include/web_server.h` publicly. Internal routes, JSON helpers, setup parsing and static-asset declarations share one private `web_server_internal.h` instead of a separate private header for every `.c` file.

## Build

For local firmware tests, the risk-based coverage plan and host/target verification boundaries, see [docs/firmware-test-plan.md](docs/firmware-test-plan.md). The project-wide validation contract, including the staged nRF54L15 software/HIL gates, is [docs/local-verification.md](docs/local-verification.md).

The sensor input/output map and its offline executable checks are in [docs/sensor-signals.md](docs/sensor-signals.md): `python tools/run_sensor_signal_checks.py`.

Install ESP-IDF v5.5.4 and open an ESP-IDF terminal in the repository directory.

When managed dependencies change:

```powershell
idf.py reconfigure
```

Build:

```powershell
idf.py build
```

Select another seismic backend:

```powershell
idf.py menuconfig
```

Then open **Seismic Acquisition** and select the required source.

## Flash and monitor

For repeatable local verification, use the tasks in `.vscode/tasks.json`:

* `Tests: nRF54L15` launches the isolated path-safe PlatformIO/Zephyr automatic
  software-test action through `nRF54L15/pio.ps1` without uploading a test
  image. Only explicitly qualified `test_sw_*` suites belong to this path.
* `nRF54L15: Flash firmware` builds and uploads the nRF54L15 image through
  `nRF54L15/pio.ps1`; it is a physical-board action and is not part of
  `Tests: All (no flash)`.
* `Tests: All (no flash)` is the no-flash project-wide entry point. It includes
  the nRF54L15 wrapper build and only qualified nRF software/native tests; an
  empty current `test_sw_*` surface is reported as `SKIPPED`. Physical nRF
  target/HIL suites are excluded even though PlatformIO also supports embedded
  tests.
* `ESP: Unity tests (flash COM4 + logs COM6)` runs the target Unity cases with
  a live COM4 preflight, an explicit test-app flash, and a bounded COM6 log
  capture.
* `ESP: Production pipeline HIL (COM4 + COM6)` builds the production image,
  prompts for the HTTPS base URL and an existing object ID, then runs the live
  acquisition/recorder/API gate with explicit COM4 flash and COM6 logging.
* `ESP: Flash production (COM4)` and `ESP: Monitor logs (COM6)` remain separate
  tasks for production firmware, each with its matching live-port preflight.

For the keypad ADC/timer-task watchlist, use the dedicated opt-in procedure in
[docs/keypad-adc-timing-hil.md](docs/keypad-adc-timing-hil.md). It measures
broker snapshot `min/avg/p95/max` and shared `ESP_TIMER_TASK` dispatch lateness
for separate AD7606 500 Hz windows; it does not change the
production defaults. Its `KEYPAD_TIMING_TELEMETRY_RESULT` confirms only
telemetry validity and is independent of the physical keypad condition;
`PHYSICAL_CONDITION` and `KEYPAD_TIMING_RUN_RESULT` report the separate HIL
gate. Performance acceptance remains `NOT_ESTABLISHED` until real-board
baselines define thresholds.

The host contracts cover the AD7606 DMA configuration; the production HIL
runner reports AD7606 startup, configured-rate frame cadence, recorder/session
metadata, exact seismic channel IDs and bounded timestamp jitter,
`/api/seismic`/`/api/status` checks (including zero AD7606 read errors and
consecutive failures) and advancing FreeRTOS heartbeats for the
acquisition, recorder and UI loops. The official Button component owns input
sampling/debounce; its physical delivery and electrical behavior are checked
separately during HIL. During recording it
also requires acquisition and recorder progress to advance. It uses the device
monotonic `runtime.now_ms` to reject task heartbeats older than the configured
`-MaxTaskHeartbeatAgeMs` (default 1000 ms). TFT pixels and
keypad/power-button responsiveness still require a human observation during the
live Measurements run; use `-RequireFullPipeline` so missing API, SD or physical
evidence is reported as `BLOCKED` instead of being mistaken for a complete pass.
After observing the live screen, keypad navigation and ordered shutdown, repeat
the explicit command with `-ConfirmTftRendering -ConfirmKeypadResponsive
-ConfirmPowerButtonResponsive` to record those three manual attestations as
`PASS`.

Use the board programming port for flashing:

```powershell
idf.py -p COM4 flash
```

In the current bench setup an external USB-to-TTL adapter can be used as a passive UART monitor while the device runs from the 2x18650 power system. The external adapter must not power the ESP32 board.

Typical passive monitor wiring:

```text
ESP32 GPIO43 TX -> USB-TTL RXD
ESP32 GND       -> USB-TTL GND
```

USB-TTL TXD and power pins are not required for log-only monitoring.

Example:

```powershell
idf.py -p COM6 monitor
```

COM numbers are development-machine specific.

## Hardware bring-up checks

A normal AD7606 start should contain messages equivalent to:

```text
AD7606_DRIVER: GPIO configured: RESET=2 CONVST=42 BUSY=47 CS=41
AD7606_DRIVER: Initializing dedicated SPI3: MISO=39 SCLK=40
AD7606_DRIVER: SPI3 initialized successfully: DMA=auto (SPI_DMA_CH_AUTO)
AD7606_DRIVER: Self-test conversion + SPI read OK
AD7606_ACQ: Started: 500 Hz, 8 channels, SPI3 dedicated, core=1
```

For a known approximately 1.6 V divider connected to V8, a representative reading is around:

```text
raw=[...,10500]
mV=[...,1602]
```

The same V8 value should appear on the LVGL Measurements screen. Exact values depend on the measured supply/reference/input voltage and must not be treated as a calibration result.

## Відкритий код та власна розробка

### Використано з open source

#### ESP32-S3 та компоненти ESP-IDF

| Компонент | Версія | Ліцензія SPDX | Посилання на джерело | Де використано |
| --- | --- | --- | --- | --- |
| ESP-IDF | 5.5.4 | [Apache-2.0](https://spdx.org/licenses/Apache-2.0.html) | [espressif/esp-idf](https://github.com/espressif/esp-idf) | Зовнішній фреймворк та інструменти збирання прошивки ESP32-S3. |
| `espressif/button` | 4.2.0 | [Apache-2.0](https://spdx.org/licenses/Apache-2.0.html) | [esp-iot-solution/button](https://github.com/espressif/esp-iot-solution/tree/master/components/button) | Клавіатура та кнопка живлення: `components/keypad`, `components/power_control`. |
| `espressif/cbor` | 0.6.1~4 | [MIT](https://spdx.org/licenses/MIT.html) | [idf-extra-components/cbor](https://github.com/espressif/idf-extra-components/tree/master/cbor) | Транзитивна залежність ESP Insights. |
| `espressif/cmake_utilities` | 0.5.3 | [Apache-2.0](https://spdx.org/licenses/Apache-2.0.html) | [esp-iot-solution/cmake_utilities](https://github.com/espressif/esp-iot-solution/tree/master/tools/cmake_utilities) | Транзитивний помічник збирання. |
| `espressif/esp-dsp` | 1.8.2 | [Apache-2.0](https://spdx.org/licenses/Apache-2.0.html) | [espressif/esp-dsp](https://github.com/espressif/esp-dsp) | Задекларований, але не використовується; залишено навмисно: закріплено тестом архітектурного контракту (`tests/python/test_architecture_refactor_contract.py`). |
| `espressif/esp_diag_data_store` | 1.1.1 | [Apache-2.0](https://spdx.org/licenses/Apache-2.0.html) | [esp-insights/esp_diag_data_store](https://github.com/espressif/esp-insights/tree/main/components/esp_diag_data_store) | Транзитивна залежність ESP Insights. |
| `espressif/esp_diagnostics` | 1.3.4 | [Apache-2.0](https://spdx.org/licenses/Apache-2.0.html) | [esp-insights/esp_diagnostics](https://github.com/espressif/esp-insights/tree/main/components/esp_diagnostics) | Транзитивна залежність ESP Insights. |
| `espressif/esp_insights` | 1.3.4 | [Apache-2.0](https://spdx.org/licenses/Apache-2.0.html) | [esp-insights/esp_insights](https://github.com/espressif/esp-insights/tree/main/components/esp_insights) | Опційний компонент `components/insights_bridge`; вимкнений у поточному `sdkconfig`. |
| `espressif/esp_lcd_ili9341` | 2.0.2 | [Apache-2.0](https://spdx.org/licenses/Apache-2.0.html) | [esp-bsp/esp_lcd_ili9341](https://github.com/espressif/esp-bsp/tree/master/components/lcd/esp_lcd_ili9341) | Драйвер TFT у `components/display`. |
| `espressif/esp_lvgl_port` | 2.9.0 | [Apache-2.0](https://spdx.org/licenses/Apache-2.0.html) | [esp-bsp/esp_lvgl_port](https://github.com/espressif/esp-bsp/tree/master/components/esp_lvgl_port) | Інтеграція LVGL з TFT. |
| `espressif/i2c_bus` | 1.5.2 | [Apache-2.0](https://spdx.org/licenses/Apache-2.0.html) | [esp-iot-solution/i2c_bus](https://github.com/espressif/esp-iot-solution/tree/master/components/i2c_bus) | Спільна шина датчиків та сервіси барометра й топології nRF. |
| `espressif/led_indicator` | 2.1.2 | [Apache-2.0](https://spdx.org/licenses/Apache-2.0.html) | [esp-iot-solution/led_indicator](https://github.com/espressif/esp-iot-solution/tree/master/components/led/led_indicator) | Адаптер індикатора; поточний драйвер плати вимкнений. |
| `espressif/led_strip` | 3.0.3 | [Apache-2.0](https://spdx.org/licenses/Apache-2.0.html) | [idf-extra-components/led_strip](https://github.com/espressif/idf-extra-components/tree/master/led_strip) | Транзитивна LED-залежність. |
| `espressif/mdns` | 1.11.3 | [Apache-2.0](https://spdx.org/licenses/Apache-2.0.html) | [esp-protocols/mdns](https://github.com/espressif/esp-protocols/tree/master/components/mdns) | mDNS у `components/wifi_manager`. |
| `espressif/rmaker_cmd_resp` | 1.0.0 | [Apache-2.0](https://spdx.org/licenses/Apache-2.0.html) | [esp-rainmaker-common/rmaker_cmd_resp](https://github.com/espressif/esp-rainmaker-common/tree/master/components/rmaker_cmd_resp) | Транзитивна залежність ESP Insights. |
| `espressif/rmaker_common` | 1.8.6 | [Apache-2.0](https://spdx.org/licenses/Apache-2.0.html) | [esp-rainmaker-common](https://github.com/espressif/esp-rainmaker-common) | Транзитивна залежність ESP Insights. |
| `espressif/rmaker_common_events` | 1.0.0 | [Apache-2.0](https://spdx.org/licenses/Apache-2.0.html) | [esp-rainmaker-common/rmaker_common_events](https://github.com/espressif/esp-rainmaker-common/tree/master/components/rmaker_common_events) | Транзитивна залежність ESP Insights. |
| `espressif/rmaker_console` | 1.0.1 | [Apache-2.0](https://spdx.org/licenses/Apache-2.0.html) | [esp-rainmaker-common/rmaker_console](https://github.com/espressif/esp-rainmaker-common/tree/master/components/rmaker_console) | Транзитивна залежність ESP Insights. |
| `espressif/rmaker_system_ctrl` | 1.0.0 | [Apache-2.0](https://spdx.org/licenses/Apache-2.0.html) | [esp-rainmaker-common/rmaker_system_ctrl](https://github.com/espressif/esp-rainmaker-common/tree/master/components/rmaker_system_ctrl) | Транзитивна залежність ESP Insights. |
| `espressif/rmaker_time_sync` | 1.0.2 | [Apache-2.0](https://spdx.org/licenses/Apache-2.0.html) | [esp-rainmaker-common/rmaker_time_sync](https://github.com/espressif/esp-rainmaker-common/tree/master/components/rmaker_time_sync) | Транзитивна залежність ESP Insights. |
| `espressif/rmaker_work_queue` | 1.0.0 | [Apache-2.0](https://spdx.org/licenses/Apache-2.0.html) | [esp-rainmaker-common/rmaker_work_queue](https://github.com/espressif/esp-rainmaker-common/tree/master/components/rmaker_work_queue) | Транзитивна залежність ESP Insights. |
| `lvgl/lvgl` | 9.6.0~1 | [MIT](https://spdx.org/licenses/MIT.html) | [lvgl/lvgl](https://github.com/lvgl/lvgl) | Інтерфейс у `components/ui`, віджет QR та шрифти TFT. |
| Montserrat Regular для LVGL | commit `cc8daf2e7085006b9c112542fc82b58afc13521d` | [OFL-1.1](https://spdx.org/licenses/OFL-1.1.html) | [JulietaUla/Montserrat](https://github.com/JulietaUla/Montserrat) | TTF SHA-256: `3e8abe50c44c82e2242e97d1ec8c0d385c4890cdc50447bcdb8605c81a38cfb2`; вбудований LVGL-шрифт версії 9.000; ліцензія збережена в `components/ui/fonts/OFL.txt`. |
| QR-Code-generator by Project Nayuki у LVGL | Окрему версію не зафіксовано; входить до LVGL 9.6.0~1 | [MIT](https://spdx.org/licenses/MIT.html) | [Nayuki QR Code generator](https://www.nayuki.io/page/qr-code-generator-library) | Рушій віджета `lv_qrcode` для Wi-Fi QR на TFT; джерело й ліцензія містяться в LVGL. |
| Вбудовані гліфи LVGL Montserrat / Font Awesome | LVGL 9.6.0~1; версію Font Awesome не зафіксовано | [OFL-1.1](https://spdx.org/licenses/OFL-1.1.html) для шрифтових гліфів; [CC-BY-4.0](https://spdx.org/licenses/CC-BY-4.0.html) для іконок Font Awesome | [LVGL](https://github.com/lvgl/lvgl), [Font Awesome](https://fontawesome.com) | Вбудовані символи `LV_SYMBOL_*` у TFT-інтерфейсі. |

#### Бібліотеки, що постачаються з ESP-IDF

| Компонент | Версія | Ліцензія SPDX | Посилання на джерело | Де використано |
| --- | --- | --- | --- | --- |
| cJSON | 1.7.19 | [MIT](https://spdx.org/licenses/MIT.html) | [DaveGamble/cJSON](https://github.com/DaveGamble/cJSON) | JSON у `object_store`, `localization_results`, `web_server` та `workflow_service`. |
| FreeRTOS-Kernel | 10.5.1 | [MIT](https://spdx.org/licenses/MIT.html) | [FreeRTOS/FreeRTOS-Kernel](https://github.com/FreeRTOS/FreeRTOS-Kernel) | Ядро задач і черг прошивки. |
| Mbed TLS | 3.6.5 | [Apache-2.0](https://spdx.org/licenses/Apache-2.0.html) OR [GPL-2.0-or-later](https://spdx.org/licenses/GPL-2.0-or-later.html) | [Mbed-TLS/mbedtls](https://github.com/Mbed-TLS/mbedtls) | ESP-TLS та HTTPS. |
| lwIP | 2.2.0, знімок розробки | [BSD-3-Clause](https://spdx.org/licenses/BSD-3-Clause.html) | [lwip-tcpip/lwip](https://github.com/lwip-tcpip/lwip) | Мережевий стек ESP-NETIF/Wi-Fi. |
| FatFs | R0.15 patch 2 | [BSD-3-Clause](https://spdx.org/licenses/BSD-3-Clause.html) | [FatFs](http://elm-chan.org/fsw/ff/) | Файлова система microSD у `components/sd_card`. |
| http-parser | 2.7.0 | [MIT](https://spdx.org/licenses/MIT.html) | [nodejs/http-parser](https://github.com/nodejs/http-parser) | HTTP-сервер ESP-IDF у `components/web_server`. |
| wpa_supplicant | 2.10 | [BSD-3-Clause](https://spdx.org/licenses/BSD-3-Clause.html) | [w1.fi/wpa_supplicant](https://w1.fi/wpa_supplicant/) | Автентифікація Wi-Fi. |
| protobuf-c | 1.4.1 | [MIT](https://spdx.org/licenses/MIT.html) | [protobuf-c/protobuf-c](https://github.com/protobuf-c/protobuf-c) | Транзитивна залежність опційної діагностики ESP Insights. |
| SPIFFS | `0.2-255-g0dbb3f71c5f6`, зазначено у `.gitmodules` ESP-IDF v5.5.4; локально не перевірено | [MIT](https://spdx.org/licenses/MIT.html) | [pellepl/spiffs](https://github.com/pellepl/spiffs) | Файлова система увімкнена в `sdkconfig`; викликів API застосунком не знайдено. |
| Бінарні бібліотеки Wi-Fi та PHY ESP-IDF | 5.5.4 | [Apache-2.0](https://spdx.org/licenses/Apache-2.0.html) | [ESP-IDF v5.5.4 components](https://github.com/espressif/esp-idf/tree/v5.5.4/components) | Закриті бінарні бібліотеки SDK для Wi-Fi та PHY. |

#### nRF54L15 / NCS

| Компонент | Версія | Ліцензія SPDX | Посилання на джерело | Де використано |
| --- | --- | --- | --- | --- |
| nRF Connect SDK / `sdk-nrf` | v3.4.0 | [LicenseRef-Nordic-5-Clause](https://github.com/nrfconnect/sdk-nrf/blob/v3.4.0/nrf/LICENSE) | [nrfconnect/sdk-nrf](https://github.com/nrfconnect/sdk-nrf) | Зовнішній SDK, зафіксований у `nRF54L15/ncs.ps1`. |
| Форк Zephyr у NCS | `ncs-v3.4.0` / Zephyr 4.4.0 | [Apache-2.0](https://spdx.org/licenses/Apache-2.0.html) | [nrfconnect/sdk-zephyr](https://github.com/nrfconnect/sdk-zephyr) | Ядро та стек Bluetooth nRF54L15. |
| nrfxlib / SoftDevice Controller | 3.4.0 | [LicenseRef-Nordic-5-Clause](https://github.com/nrfconnect/sdk-nrfxlib) | [nrfconnect/sdk-nrfxlib](https://github.com/nrfconnect/sdk-nrfxlib) | Бінарний контролер Bluetooth, увімкнений у `prj.conf`. |
| hal_nordic | commit `18da0cc9726f8759c627dba3180b3ba9294e433c` | [BSD-3-Clause](https://spdx.org/licenses/BSD-3-Clause.html) для nrfx; [LicenseRef-Nordic-5-Clause](https://github.com/nrfconnect/sdk-nrf/blob/v3.4.0/nrf/LICENSE) для модуля | [zephyrproject-rtos/hal_nordic](https://github.com/zephyrproject-rtos/hal_nordic) | Драйвери периферії. |
| CMSIS | commit `512cc7e895e8491696b61f7ba8066b4a182569b8` | [Apache-2.0](https://spdx.org/licenses/Apache-2.0.html) | [zephyrproject-rtos/cmsis](https://github.com/zephyrproject-rtos/cmsis) | Підтримка ядра Cortex-M. |

#### Вебінтерфейс та пакет активів

| Компонент | Версія | Ліцензія SPDX | Посилання на джерело | Де використано |
| --- | --- | --- | --- | --- |
| `react` | 19.2.8 | [MIT](https://spdx.org/licenses/MIT.html) | [react/react](https://github.com/react/react) | Середовище виконання інтерфейсу у `webui/src/main.tsx`; входить до пакета WebUI. |
| `react-dom` | 19.2.8 | [MIT](https://spdx.org/licenses/MIT.html) | [react/react](https://github.com/react/react) | Монтування інтерфейсу та звітів; входить до пакета WebUI. |
| `three` | 0.185.1 | [MIT](https://spdx.org/licenses/MIT.html) | [mrdoob/three.js](https://github.com/mrdoob/three.js) | 3D-рендеринг і керування сценою. |
| `@react-three/fiber` | 9.7.0 | [MIT](https://spdx.org/licenses/MIT.html) | [pmndrs/react-three-fiber](https://github.com/pmndrs/react-three-fiber) | 3D-види хмари точок, сцени та локалізації. |
| `js-aruco2` | 2.0.0 | [MIT](https://spdx.org/licenses/MIT.html) | [damianofalcioni/js-aruco2](https://github.com/damianofalcioni/js-aruco2) | Детектор AprilTag та JS-словник; входить до WebUI. |
| `@fontsource/montserrat` WOFF2 | 5.3.0 | [OFL-1.1](https://spdx.org/licenses/OFL-1.1.html) | [fontsource Montserrat](https://github.com/fontsource/font-files/tree/main/fonts/google/montserrat) | Латиничні та кириличні файли WOFF2 вбудовано до частини пакета WebUI для друку звіту. |
| Словник AprilTag `tag36h11` | Версію сімейства не зафіксовано; дані зі словника `js-aruco2` 2.0.0 | [BSD-2-Clause](https://spdx.org/licenses/BSD-2-Clause.html) | [AprilRobotics/apriltag](https://github.com/AprilRobotics/apriltag) | Дані словника в `webui/src/xr/fiducial/jsAruco2.ts`; детектор AprilTag мовою C у WebUI не виявлено. |

#### Інструменти розробки та збирання

| Компонент | Версія | Ліцензія SPDX | Посилання на джерело | Де використано |
| --- | --- | --- | --- | --- |
| `@types/react` | 19.2.18; у маніфесті `^19.2.0` | [MIT](https://spdx.org/licenses/MIT.html) | [DefinitelyTyped/types/react](https://github.com/DefinitelyTyped/DefinitelyTyped/tree/master/types/react) | Типи для компіляції WebUI. |
| `@types/react-dom` | 19.2.5; у маніфесті `^19.2.0` | [MIT](https://spdx.org/licenses/MIT.html) | [DefinitelyTyped/types/react-dom](https://github.com/DefinitelyTyped/DefinitelyTyped/tree/master/types/react-dom) | Типи для компіляції WebUI. |
| `@types/three` | 0.185.4 | [MIT](https://spdx.org/licenses/MIT.html) | [DefinitelyTyped/types/three](https://github.com/DefinitelyTyped/DefinitelyTyped/tree/master/types/three) | Типи для компіляції WebUI. |
| `@vitejs/plugin-react` | 6.1.1 | [MIT](https://spdx.org/licenses/MIT.html) | [vitejs/vite-plugin-react](https://github.com/vitejs/vite-plugin-react) | Плагін Vite для розробки та збирання WebUI. |
| `@vitest/coverage-v8` | 4.1.11 | [MIT](https://spdx.org/licenses/MIT.html) | [vitest-dev/vitest](https://github.com/vitest-dev/vitest) | Збір покриття для тестів WebUI. |
| `fontkit` | 2.0.4 | [MIT](https://spdx.org/licenses/MIT.html) | [foliojs/fontkit](https://github.com/foliojs/fontkit) | Перевірка шрифтів у тестах звітів. |
| `react-test-renderer` | 19.2.8 | [MIT](https://spdx.org/licenses/MIT.html) | [react/react](https://github.com/react/react) | Рендерер компонентів для тестів WebUI. |
| TypeScript | 7.0.2 | [Apache-2.0](https://spdx.org/licenses/Apache-2.0.html) | [microsoft/TypeScript](https://github.com/microsoft/TypeScript) | Перевірка типів і підготовка WebUI. |
| Vite | 8.2.2 | [MIT](https://spdx.org/licenses/MIT.html) | [vitejs/vite](https://github.com/vitejs/vite) | Сервер розробки та збирач production-пакета WebUI. |
| Vitest | 4.1.11 | [MIT](https://spdx.org/licenses/MIT.html) | [vitest-dev/vitest](https://github.com/vitest-dev/vitest) | Засіб запуску тестів WebUI. |
| `lv_font_conv` | 1.5.3 | [MIT](https://spdx.org/licenses/MIT.html), за метаданими upstream; локальну ліцензію не підтверджено | [lvgl/lv_font_conv](https://github.com/lvgl/lv_font_conv) | Конвертує TTF у LVGL C-шрифт. |
| Segno | 1.6.6 в інструкціях генератора; пакет не був встановлений під час аудиту | [BSD-3-Clause](https://spdx.org/licenses/BSD-3-Clause.html) | [heuer/segno](https://github.com/heuer/segno) | Генерація еталонних QR-векторів для тестових контрольних даних; залежністю часу виконання не є. |
| pytest | 9.0.1 у середовищі аудиту; версію проєктом не зафіксовано | [MIT](https://spdx.org/licenses/MIT.html) | [pytest-dev/pytest](https://github.com/pytest-dev/pytest) | Запуск Python-тестів у host-середовищі. |
| ESP-IDF Component Manager | 2.4.11 у firmware-аудиті; у середовищах WebUI-аудиту й поточному середовищі не встановлений; проєктом не зафіксовано | [Apache-2.0](https://spdx.org/licenses/Apache-2.0.html) | [espressif/idf-component-manager](https://github.com/espressif/idf-component-manager) | Помічник для host-середовища: `tools/prepare_host_button.py`. |
| Платформа Seeed для PlatformIO | commit `1ec1287f8e4bc4067a6fd593991e36875aef989f` | [Apache-2.0](https://spdx.org/licenses/Apache-2.0.html) | [platform-seeedboards](https://github.com/Seeed-Studio/platform-seeedboards) | Лише опційне успадковане перевірочне збирання для nRF54L15. |
| Espressif esp-emu (esp-emulator) | v0.49.0 | [Apache-2.0](https://spdx.org/licenses/Apache-2.0.html) | [espressif/esp-emulator](https://github.com/espressif/esp-emulator); [LICENSE](https://raw.githubusercontent.com/espressif/esp-emulator/main/LICENSE) | Лише для розробки та тестів: `tools/run_emulator_smoke.py` у WSL. Готовий Linux x86_64 бінарник завантажується з GitHub-релізу проєкту з перевіркою SHA-256; не входить до прошивки, не зберігається в репозиторії та не модифікувався. Ліцензію бінарника окремо не перевірено; припускається, що вона відповідає ліцензії репозиторію. |
| WSL2 Ubuntu 26.04 та west | Ubuntu 26.04; версію west не вказано | Не вказано окремо | [Ubuntu](https://ubuntu.com/), [west](https://github.com/zephyrproject-rtos/west) | Інструменти host-середовища лише для розробки: WSL2 Ubuntu 26.04 з apt-пакетами `gcc`, `cmake`, `ninja`, `dtc`, `python3-venv` та west для `native_sim`. |

Транзитивні npm-залежності повністю не аудіювалися; точні версії, яких немає у маніфестах, файлах блокування або звітах, позначено як непідтверджені.

#### Файли KiCad

- `kicad/XIAO_Series_SCH_Symbols/` — бібліотека символів Seeed Studio XIAO; сторонній матеріал.
- `kicad/202004288;XIAO nRF54L15 v1.0_SCH&PCB/` — еталонна схема та PCB Seeed Studio; сторонній матеріал.
- `kicad/old/` — архівна копія проєкту, що посилається на бібліотеку Seeed.
- `kicad/esp32_geophone_source_locator/` — власна схема та PCB, що використовують символи Seeed.

У файлах Seeed ліцензійних приміток не знайдено. Ліцензія НЕ ПІДТВЕРДЖЕНА і має бути уточнена у Seeed Studio перед поширенням. Зовнішні 3D-моделі підключені за шляхами, але в репозиторії відсутні.

### Розроблено самостійно

| Код | Власна розробка |
| --- | --- |
| `components/*`, `main/` | Компоненти прошивки та код застосунку ESP32. |
| `nRF54L15/src/`, `nRF54L15/include/` | Власний код застосунку nRF54L15 та заголовки. |
| `nRF54L15/src/nrf_bmi160.c`, `nRF54L15/src/nrf_bme280.c` | Власні драйвери BMI160 і BME280 на рівні регістрів; сторонні драйвери Bosch не копіювалися. |
| `kicad/esp32_geophone_source_locator/` | Схема та PCB — власна розробка, окрім використаних символів Seeed Studio. |
| `webui/src/` | Власний код інтерфейсу WebUI. |
| `webui/src/domain/qrCode.ts` | Власна реалізація QR-коду для звітів; окрему npm QR-бібліотеку не підключено. |
| `tools/apriltag_sheet/apriltag_sheet.py`, `tools/apriltag_sheet/model.py` | Власний генератор PDF-листа AprilTag на стандартній бібліотеці Python; словникові дані `tag36h11` походять зі стороннього джерела, наведеного вище. |

Ліцензія проєкту: [CC0-1.0 / Public Domain](LICENSE); сторонні повідомлення: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Project status

Implemented foundations include:

- componentized ESP-IDF architecture;
- centralized application task/core/priority policy;
- LVGL 9 TFT UI with five-button keypad navigation;
- one UI screen-driver registry instead of duplicated screen switches;
- live AD7606 transport/acquisition/channel diagnostics on TFT;
- microSD object storage and recovery;
- safe power-down flow;
- Wi-Fi and local web server;
- unified web component-private interface convention;
- simultaneous AD7606 acquisition;
- dedicated SPI3 acquisition bus;
- Core 0 / Core 1 task separation;
- CONVST-time frame timestamps;
- non-blocking waveform snapshot architecture;
- common acquisition-backend driver interface/registry;
- reusable 3D TDOA localization solver;
- `ARCHITECTURE.md` with runtime and real-time design rules.

Still required for an end-to-end source-location system:

- final low-noise geophone analog front ends;
- plain nRF54L15 remote-node external GY-BMI160 verticality integration,
  local BME280 aggregation on the remote nRF, and Bluetooth 6 Channel Sounding
  geometry integration;
- signal filtering / preprocessing;
- event detection and per-channel arrival-time picking;
- measured/modelled propagation-speed strategy for rubble;
- localization integration with actual events and quality/rejection criteria;
- deterministic hardware-timed acquisition if required by the measured timing error budget;
- calibration and quantified measurement accuracy.

This repository is a development/diploma prototype and is not a certified rescue instrument.
