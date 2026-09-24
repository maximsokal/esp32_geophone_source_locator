# ESP32 Geophone Source Locator

ESP32-S3 diploma prototype for seismic search under rubble. The firmware acquires multiple geophone channels simultaneously, stores object/measurement data on microSD, exposes a local web interface, provides a local TFT UI, and now contains a reusable 3D TDOA geometry solver for the later localization pipeline.

The project targets **ESP-IDF v5.5.4** on an ESP32-S3 N16R8 board.

For the component/runtime design, task ownership and extension rules, see [ARCHITECTURE.md](ARCHITECTURE.md).

## Current hardware architecture

### ESP32-S3

The controller is an ESP32-S3 with 16 MB flash and 8 MB octal PSRAM.

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
| Temporary ESP ADC bench/divider input | 1 |

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
- ESP ADC bench acquisition source;
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
- `ESP32-S3 ADC geophone bench input` - temporary direct ADC bench mode;
- `Synthetic test waveform` - software-only waveform source.

Each backend implements the same private `seismic_source_driver_t` interface. The acquisition orchestrator itself is backend-agnostic; compile-time selection is isolated in the source registry. Runtime source switching is intentionally not exposed yet.

Current backend files:

- `seismic_source_ad7606.c`;
- `seismic_source_esp_adc.c`;
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
for separate AD7606 and ESP ADC bench 500 Hz windows; it does not change the
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
