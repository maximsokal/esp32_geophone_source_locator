# Architecture

This document describes the runtime ownership, real-time boundaries, component interfaces, extension points, and the **physical prototype wiring** of `esp32_geophone_source_locator`.

The design target is ESP32-S3 N16R8 on ESP-IDF v5.5.4. The firmware is split into ESP-IDF components rather than one application translation unit. Public headers live under each component's `include/` directory; multi-file component-private state and contracts use private headers inside the component directory.

> **Hardware safety rule:** the GPIO/wiring ledger in this document is part of the project architecture, not an informal note. If the prototype is disconnected for another homework or bench task, reconnect it from this ledger. Do not reassign a listed GPIO without updating both `components/board_config/include/board_config.h` and this document in the same change.

## Runtime boot sequence

`main/main.c` delegates boot and shutdown to the application orchestration layer.

```text
app_main
  -> app_startup_run
       -> app_event_bus_init
       -> power_control_init
       -> device_status_init
       -> ina3221_monitor_start (non-fatal if hardware is absent)
       -> seismic_buffer_init
       -> adc_service_init
       -> keypad_init
       -> SPI2 init
       -> isolated SD startup check
       -> display hardware init
       -> LVGL boot UI
       -> storage_manager init
       -> object recovery
       -> object store
       -> object service
       -> storage service
       -> Wi-Fi manager
       -> HTTP server
       -> interactive LVGL UI
       -> keypad event monitor
       -> selected seismic source start
  -> wait for shutdown request
  -> app_shutdown_run
```

Each major boot stage is timed and logged so slow storage or hardware bring-up is visible in the serial log.

## Core ownership

The project deliberately separates service work from acquisition work.

### Core 0 - services

- LVGL handler task (`esp_lvgl_port`)
- UI event worker
- analog keypad monitor
- power-button monitor
- INA3221 low-rate power monitor
- object service
- storage service
- Wi-Fi / HTTP-facing application work

### Core 1 - real-time side

- AD7606 acquisition
- ESP ADC bench acquisition source
- synthetic seismic source
- future preprocessing / arrival picking
- future localization worker if it becomes a continuous task

The dedicated AD7606 SPI3 bus is initialized by the Core 1 acquisition worker so SPI interrupt affinity remains on the acquisition side.

## Task policy

Application-owned task stack sizes, priorities and core assignments are centralized in `components/app_config/include/app_task_config.h`.

Current policy:

| Task | Core | Priority | Stack |
| --- | ---: | ---: | ---: |
| AD7606 acquisition | 1 | 7 | 4096 |
| ESP ADC bench | 1 | 6 | 4096 |
| Synthetic seismic source | 1 | 4 | 3072 |
| UI event worker | 0 | 6 | 6144 |
| LVGL handler | 0 | 4 | 7168 |
| Power button | 0 | 5 | 3072 |
| Keypad | 0 | 4 | 8192 |
| Storage service | 0 | 3 | 8192 |
| Object service | 0 | 3 | 8192 |
| INA3221 monitor | 0 | 1 | 4096 |

Do not introduce new application tasks with ad-hoc priorities. Add them to `app_task_config.h` so the whole scheduling hierarchy stays reviewable in one place.

## Physical prototype wiring ledger

`components/board_config/include/board_config.h` is the software source of truth for application GPIO allocation. This section adds the physical destination, direction and bench-only wiring that cannot be recovered from a GPIO number alone.

### ESP32-S3 GPIO map - current prototype

| ESP32-S3 pin | Direction | Physical connection | Purpose / do-not-forget note |
| --- | --- | --- | --- |
| **GPIO1** | analog input | **Temporary geophone/ADC bench voltage-divider node** | **Still physically present. Reserved; do not reuse for AD7606 or homework wiring while the divider remains connected.** This is the ESP32-S3 ADC bench input (ADC1 channel 0). Exact divider resistor values/topology are not currently encoded anywhere in the repository, so they must be measured/recorded before this temporary divider is dismantled rather than guessed later. |
| **GPIO2** | output | AD7606 `RESET` | Hardware reset line for the AD7606 module. |
| **GPIO8** | analog input | Keyestudio KS0466 5-button keypad analog signal output | One ADC line represents all five buttons through the keypad's resistor ladder. |
| **GPIO9** | bidirectional open-drain | INA3221 `SDA` | Sensor I2C data. INA3221 monitor currently owns/creates this bus. |
| **GPIO10** | output/open-drain | INA3221 `SCL` | Sensor I2C clock, 100 kHz. |
| **GPIO11** | output | TFT `SDI/MOSI` **and** microSD module `MOSI` | Shared SPI2 MOSI. Both devices share the bus and use separate CS lines. |
| **GPIO12** | output | TFT `SCK` **and** microSD module `SCK` | Shared SPI2 clock. |
| **GPIO13** | input | TFT `SDO/MISO` **and** microSD module `MISO` | Shared SPI2 MISO. |
| **GPIO14** | output | TFT `CS` | ILI9341 display chip select. |
| **GPIO15** | output | TFT `DC` | ILI9341 data/command select. |
| **GPIO16** | output | TFT `RESET` | ILI9341 hardware reset. |
| **GPIO17** | output | microSD module `CS` | SD card SPI chip select. |
| **GPIO18** | output | Power-control module `OFF` control input | Normal level is LOW. After safe shutdown the firmware drives a **200 ms HIGH pulse** to request physical power cut. Do not connect this line to an unrelated peripheral. |
| **GPIO21** | input | External power-button sense line | Digital input used by the power-button monitor. ESP32 internal pull-up/pull-down is disabled, therefore the external power-control/button circuit must provide a defined logic level. |
| **GPIO39** | input | AD7606 `DOUTA/DB7` | Dedicated SPI3 MISO/data output from AD7606. |
| **GPIO40** | output | AD7606 `RD/SCLK` | Dedicated SPI3 clock. |
| **GPIO41** | output | AD7606 `CS` **plus external 10 kOhm pull-up to 3.3 V** | `CS` is active LOW. The 10 kOhm resistor keeps the ADC deselected while ESP32 is resetting/booting and GPIO41 is not yet actively driven. Firmware then drives CS HIGH before handing the pin to SPI3. **Do not remove this pull-up when rewiring.** |
| **GPIO42** | output | AD7606 `CVA` + `CVB`, physically tied together | One rising edge starts the simultaneous conversion of all eight AD7606 channels. Both CONVST inputs must remain tied to this GPIO for simultaneous sampling. |
| **GPIO43** | output | ESP32-S3 UART0 TX -> external CH340 `RXD` for passive serial monitoring | Used for battery-only COM6 logging. In passive-monitor setup only ESP32 TX -> CH340 RXD and common GND are used. |
| **GPIO44** | input / reserved | UART0 RX; **not connected to CH340 TXD in passive-monitor setup** | Leave disconnected during passive COM6 monitoring so the CH340 cannot drive the ESP32. |
| **GPIO47** | input | AD7606 `BUSY` | Conversion-status input. Firmware waits for BUSY release before SPI readout. |

All other ESP32-S3 GPIOs are currently **unallocated by `board_config.h`** unless another documented bench experiment explicitly says otherwise. Treat that as "free in the current firmware", not as permission to move one of the connections above.

### Physical resistor / temporary analog wiring notes

#### GPIO41 AD7606 CS pull-up - permanent prototype requirement

```text
3.3 V
  |
 [10 kOhm]
  |
  +------ GPIO41 ------ AD7606 CS
```

The resistor is a **pull-up**, not a series resistor. AD7606 CS is active LOW. During ESP32 reset, boot and the interval before `ad7606_driver` configures GPIO41, the ESP32 pin can be high-impedance. Without an external pull-up the CS line could float and the ADC could be selected unintentionally. The 10 kOhm pull-up establishes the safe hardware state independently of firmware. Once initialized, the ESP32 drives the same line normally and SPI3 controls CS during transactions.

#### GPIO1 temporary voltage divider - currently still connected

GPIO1 remains physically occupied by the earlier temporary ADC/geophone bench divider and is deliberately reserved in `board_config.h` so later AD7606 work does not silently reuse it.

```text
external / bench signal
       |
   [temporary divider]
       |
       +------ GPIO1 / ADC1_CH0
       |
      GND / divider return
```

**Important:** the repository currently proves that the divider exists and that GPIO1 is its ADC node, but it does **not** contain trustworthy values for the divider resistors or enough information to reconstruct which exact resistor is on each side. Those values must not be invented. Before removing the divider, record its two resistor values and exact connection here (or from a clear photo/multimeter check), then replace this paragraph with the exact schematic. Until then, GPIO1 stays reserved.

#### INA3221 power monitor - current prototype wiring

The INA3221 board is powered from the ESP32 3.3 V rail and uses address `0x40` because `A0` is tied to GND.

```text
INA3221          ESP32-S3
-------------------------
VS       ------  3V3
GND      ------  GND
SDA      ------  GPIO9
SCL      ------  GPIO10
A0       ------  GND

PV       ------  NC
CRI      ------  NC
WAR      ------  NC
TC       ------  NC
```

The three measurement branches are fixed in firmware as:

| Software channel | INA3221 physical channel | Branch | Direction |
| --- | --- | --- | --- |
| CH0 | Channel 1 | Battery / 18650 branch | source `VI1+` -> onboard `R100` 0.1 Ohm -> load `VI1-` |
| CH1 | Channel 2 | ESP32 5 V rail | source `VI2+` -> onboard `R100` 0.1 Ohm -> ESP32/load `VI2-` |
| CH2 | Channel 3 | AD7606 5 V rail | source `VI3+` -> onboard `R100` 0.1 Ohm -> AD7606/load `VI3-`; may remain unpowered until AD7606 supply wiring is added |

With this orientation, positive CH0 current means battery discharge (battery -> load) and negative CH0 current means charging current (load/charger -> battery). INA3221 bus voltage is measured on `VIx-`; firmware adds the signed shunt drop for CH0 to estimate the actual battery-side voltage at `VI1+`.

Battery percentage is intentionally an **estimated 1S Li-ion voltage state-of-charge**, not coulomb counting. It is only marked valid when CH0 source voltage is in the plausible 1S range. The current bench condition with no 18650 installed can put CH0 near 5 V while USB powers the unit; firmware treats that as **battery absent**, not as a 100% battery.

### Debug / flashing wiring

- **COM4 / USB-C:** normal ESP-IDF flashing and USB-connected monitoring path.
- **COM6 / external CH340:** passive battery-only monitor. Connect `GPIO43 (ESP TX)` -> `CH340 RXD` and `GND` -> `GND`.
- In passive COM6 mode, **CH340 TXD is disconnected** and **CH340 VCC is disconnected**.
- **Never flash through COM6.** Flash through COM4/USB only.

### Ground and power notes that must survive disassembly

- ESP32-S3, TFT, microSD, INA3221, AD7606, keypad, power-control board and any external serial monitor used for signal observation require a **common reference GND** where they exchange logic signals.
- INA3221 `VS` is connected to ESP32 **3.3 V**; its measured bus rails may be around 5 V because bus-voltage inputs are separate from `VS`.
- The GPIO41 pull-up terminates at the ESP32 **3.3 V** rail.
- The CH340 VCC pin stays disconnected in passive battery-monitor mode; only RXD and GND are connected.
- Exact present VCC rail routing for every external module is not represented by firmware GPIO definitions. Before a complete prototype teardown, add the measured/verified module power wiring here rather than reconstructing it from memory.

## Hardware bus ownership

### I2C0 - INA3221 power telemetry

```text
GPIO9  SDA <-> INA3221 SDA
GPIO10 SCL  -> INA3221 SCL
address       0x40 (A0 tied to GND)
```

`ina3221_monitor` owns the current sensor I2C master bus, configured at 100 kHz. INA3221 runs continuous shunt+bus conversion on all three channels with hardware averaging x16 and 1.1 ms conversion time. A low-priority Core 0 task publishes one status update per second. Missing/faulty INA3221 hardware invalidates power telemetry and is retried without rebooting the device.

### SPI2 - display and SD

```text
GPIO11 MOSI -> TFT SDI/MOSI + SD MOSI
GPIO12 SCLK -> TFT SCK      + SD SCK
GPIO13 MISO <- TFT SDO/MISO + SD MISO
GPIO14      -> TFT CS
GPIO15      -> TFT DC
GPIO16      -> TFT RESET
GPIO17      -> SD CS
```

The SD card stays at 2 MHz on the current prototype because higher rates were not stable on the physical assembly.

### SPI3 - AD7606 only

```text
GPIO39 <- AD7606 DOUTA/DB7
GPIO40 -> AD7606 RD/SCLK
GPIO41 -> AD7606 CS        + 10 kOhm pull-up to 3.3 V
GPIO42 -> AD7606 CVA + CVB (tied together)
GPIO47 <- AD7606 BUSY
GPIO2  -> AD7606 RESET
```

SPI3 is read-only for this AD7606 connection; MOSI is intentionally unused.

Current AD7606 acquisition assumptions are 8 channels, SPI mode 2, 1 MHz bus clock, +/-5 V input range and hardware oversampling x1. The physical RANGE / OS0 / OS1 / OS2 module straps must continue to match those assumptions when the board is rewired.

## Seismic acquisition interface

`seismic_acquisition` is the orchestration layer. It does not contain source-specific start/stop/status logic.

Each backend implements one private `seismic_source_driver_t` descriptor:

```c
typedef struct {
    const char *name;
    const char *transport_name;
    uint32_t sample_rate_hz;
    uint8_t channel_count;
    int core_id;
    esp_err_t (*start)(void);
    void (*stop)(void);
    esp_err_t (*populate_snapshot)(seismic_acquisition_snapshot_t *snapshot);
} seismic_source_driver_t;
```

Current descriptors are:

- `seismic_source_test.c`
- `seismic_source_esp_adc.c`
- `seismic_source_ad7606.c`

`seismic_source_registry.c` performs the compile-time Kconfig selection. Runtime source switching is intentionally not exposed yet; changing the selected backend requires a firmware configuration/build change. Adding a backend should not require editing the acquisition orchestrator.

ESP-IDF 5.5.4 resolves component dependencies before `sdkconfig` is available, so all backend components can appear in the CMake dependency graph. The source registry still references only the selected backend, and linker verification is used to ensure unselected backend code is absent from the final ELF.

## AD7606 sample timing

The AD7606 provides simultaneous sampling for all eight channels.

The software sequence is:

```text
periodic acquisition wake
  -> CONVST rising edge
  -> timestamp captured immediately at the sampling event
  -> CONVST pulse completes
  -> BUSY completion
  -> SPI3 read V1..V8
  -> publish acquisition status
  -> push selected channels into seismic ring buffer
```

The frame timestamp therefore represents the real simultaneous conversion trigger, not the later completion of SPI readout.

The current periodic wake still uses `esp_timer` plus a task notification. That is suitable for the present bench stage. If later TDOA error budgeting shows that inter-sample scheduling jitter is material, the timing source must be upgraded to a hardware-timed CONVST path rather than merely changing where timestamps are read.

## Seismic ring buffer

`seismic_buffer` has one acquisition producer and potentially slow readers such as HTTP clients.

Readers must never hold a long mutex that blocks the producer while hundreds of samples are copied. The current implementation uses:

- a short metadata critical section;
- per-slot sequence publication;
- optimistic reader retries if a producer wraps over a slot being read.

This keeps waveform snapshot latency on the consumer side instead of injecting it into the acquisition path.

## LVGL UI architecture

The display component owns only the physical ILI9341 / `esp_lcd` layer. Widget layout, text, QR rendering, navigation and refresh logic belong to LVGL through `esp_lvgl_port`.

```text
ILI9341
  -> esp_lcd
  -> esp_lvgl_port
  -> LVGL 9
  -> ui component
```

Screen routing uses one `ui_screen_driver_t` registry instead of repeated enum/switch dispatch:

```c
typedef struct {
    const char *name;
    const char *home_label;
    void (*open)(void);
    void (*render)(void);
    void (*handle_key)(keypad_key_t key);
    void (*refresh)(void);
} ui_screen_driver_t;
```

The Home menu is generated from the same registry, so adding a screen cannot silently update rendering but forget input routing or the menu label.

Current screens:

- Home
- Objects
- Measurements
- Storage
- Wi-Fi / Web
- System

The Measurements screen consumes `seismic_acquisition_snapshot_t`. It never touches AD7606 GPIO or SPI functions directly.

## Storage ownership and shutdown

Storage calls are serialized through `storage_manager` / `storage_service`. SD lifecycle state belongs to `sd_card`.

Safe shutdown order:

```text
keypad monitor
  -> INA3221 monitor / I2C release
  -> UI event task / LVGL
  -> storage service
  -> object service
  -> seismic acquisition
  -> HTTP server / Wi-Fi
  -> storage manager
  -> FAT SD unmount
  -> display disable/deinit
  -> power-off pulse
```

The SD handle is cleared only after a successful FAT unmount so software state does not claim the card is gone if unmount failed.

## TDOA localization boundary

A reusable 3D TDOA solver now lives in the independent `localization` component.

`localization_tdoa_solve()` accepts:

- 4..8 sensor positions in meters;
- one arrival timestamp per sensor for the same detected event;
- propagation speed in m/s;
- an optional initial source-position guess.

It solves the nonlinear TDOA equations with damped Gauss-Newton iterations and reports:

- estimated `(x, y, z)`;
- estimated emission time;
- RMS timing residual;
- iteration count;
- convergence status.

The solver rejects degenerate 3D sensor geometry. At least four non-coplanar sensor positions are required for a 3D solution.

This component deliberately does **not** decide where an event begins in raw geophone data. The future signal-processing pipeline must provide reliable arrival picks first:

```text
AD7606 frames
  -> preprocessing / filtering
  -> event detection
  -> per-channel arrival-time picking
  -> sensor positions / heights
  -> localization_tdoa_solve
  -> source estimate + residual/quality
```

Keeping arrival picking and geometry solving separate allows both stages to be bench-tested independently.

## Web component convention

The public web API is only `include/web_server.h`. Internal route/JSON/setup helpers are implementation details of the `web_server` component and should use component-private declarations, not grow additional public headers.

## Extension rules

When adding hardware or features:

1. Put hardware ownership in one component.
2. Expose a small public API; keep implementation state private.
3. Add new application task priorities/core assignments to `app_task_config.h`.
4. Do not let UI/web code call low-level ADC/SPI functions directly.
5. Keep Core 0 consumers from blocking Core 1 acquisition.
6. Add new UI screens through the screen registry.
7. Add new acquisition backends through `seismic_source_driver_t` and the source registry.
8. Keep raw arrival detection separate from the localization solver.
9. Preserve safe shutdown ordering for every new producer or storage writer.
10. Every GPIO reassignment or physical resistor/strap change must update the wiring ledger above in the same commit.
