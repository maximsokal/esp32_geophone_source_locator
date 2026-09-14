# Architecture

This document describes the runtime ownership, real-time boundaries, component interfaces, extension points, and the **physical prototype wiring** of `esp32_geophone_source_locator`.

The design target is ESP32-S3 N16R8 on ESP-IDF v5.5.4. The firmware is split into ESP-IDF components rather than one application translation unit. Public headers live under each component's `include/` directory; multi-file component-private state and contracts use private headers inside the component directory.

> **Hardware safety rule:** the GPIO/wiring ledger in this document is part of the project architecture, not an informal note. If the prototype is disconnected for another homework or bench task, reconnect it from this ledger. Do not reassign a listed GPIO without updating both `components/board_config/include/board_config.h` and this document in the same change.

## Runtime boot sequence

`main/main.c` delegates boot and shutdown to the application orchestration layer.

```text
app_main
  -> app_startup_run
       -> app_events_init
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
        -> Wi-Fi manager init
        -> interactive LVGL UI + APP_EVENT subscriber
        -> HTTP server
        -> Wi-Fi manager start
       -> selected seismic source start
  -> wait for shutdown request
  -> app_shutdown_run
```

Each major boot stage is timed and logged so slow storage or hardware bring-up is visible in the serial log.

## Core ownership

The project deliberately separates service work from acquisition work.

### Core 0 - services

- LVGL handler task (`esp_lvgl_port`)
- ESP-IDF APP_EVENT user-loop task
- official Button component service for keypad and power-button sampling
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
| APP_EVENT user loop | 0 | 6 | 6144 |
| Optional Insights retry worker | 0 | 1 | 4096 |
| LVGL handler | 0 | 4 | 7168 |
| Storage service | 0 | 3 | 8192 |
| Object service | 0 | 3 | 8192 |
| Measurement service | 0 | 3 | 4096 |
| Seismic recorder | 0 | 2 | 4096 |
| INA3221 monitor | 0 | 1 | 4096 |

Do not introduce new application tasks with ad-hoc priorities. Add them to `app_task_config.h` so the whole scheduling hierarchy stays reviewable in one place. When the opt-in ESP Insights profile is enabled, its APP_EVENT connection handler only notifies the optional retry worker above and returns; the worker performs a failed `esp_insights_init()` retry outside the shared application event loop. The baseline production profile does not create this task.

## Physical prototype wiring ledger

`components/board_config/include/board_config.h` is the software source of truth for application GPIO allocation. This section adds the physical destination, direction and bench-only wiring that cannot be recovered from a GPIO number alone.

### ESP32-S3 GPIO map - current prototype

| ESP32-S3 pin | Direction | Physical connection | Purpose / do-not-forget note |
| --- | --- | --- | --- |
| **GPIO1** | analog input | **Temporary geophone/ADC bench voltage-divider node** | **Still physically present. Reserved; do not reuse for AD7606 or homework wiring while the divider remains connected.** This is the ESP32-S3 ADC bench input (ADC1 channel 0). Exact divider resistor values/topology are not currently encoded anywhere in the repository, so they must be measured/recorded before this temporary divider is dismantled rather than guessed later. |
| **GPIO2** | output | AD7606 `RESET` | Hardware reset line for the AD7606 module. |
| **GPIO8** | analog input | Keyestudio KS0466 5-button keypad analog signal output | One ADC line represents all five buttons through the keypad's resistor ladder. |
| **GPIO9** | bidirectional open-drain | INA3221 `SDA` | Sensor I2C data. `sensor_bus` owns shared I2C0. |
| **GPIO10** | output/open-drain | INA3221 `SCL` | Sensor I2C clock, 100 kHz; shared-bus owner is `sensor_bus`. |
| **GPIO11** | output | TFT `SDI/MOSI` **and** microSD module `MOSI` | Shared SPI2 MOSI. Both devices share the bus and use separate CS lines. |
| **GPIO12** | output | TFT `SCK` **and** microSD module `SCK` | Shared SPI2 clock. |
| **GPIO13** | input | TFT `SDO/MISO` **and** microSD module `MISO` | Shared SPI2 MISO. |
| **GPIO14** | output | TFT `CS` | ILI9341 display chip select. |
| **GPIO15** | output | TFT `DC` | ILI9341 data/command select. |
| **GPIO16** | output | TFT `RESET` | ILI9341 hardware reset. |
| **GPIO17** | output | microSD module `CS` | SD card SPI chip select. |
| **GPIO18** | output | Power-control module `OFF` control input | Normal level is LOW. After safe shutdown the firmware drives a **200 ms HIGH pulse** to request physical power cut. Do not connect this line to an unrelated peripheral. |
| **GPIO21** | input | External power-button sense line | Official GPIO Button adapter input, active-high. Internal pull-up/pull-down is disabled, therefore the external power-control/button circuit must provide a defined logic level. |
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

## nRF54L15 subproject and distributed sensor topology

`nRF54L15/` is a separate PlatformIO + Zephyr project inside this repository.
The repository root remains an ESP-IDF project for the ESP32-S3; the WebUI and
the nRF54L15 project remain independent. Do not add a root-level
`platformio.ini`, and do not make the ESP-IDF build depend on the nRF build.

The nRF project currently has one production PlatformIO environment,
`seeed-xiao-nrf54l15`, using the Seeed board ID `seeed-xiao-nrf54l15` and the
Zephyr application target `xiao_nrf54l15/nrf54l15/cpuapp`. All nRF54L15 boards
must use one source tree, one production environment, one implementation and
one production firmware binary. There are no compile-time `central` and
`remote` firmware variants; node identity, topology and the current wireless
role are runtime state. A future native/host-only test environment is allowed
only when needed to execute software tests without hardware; it must not
become a second production firmware variant.

### Windows build boundary

The checkout path contains spaces and the current Seeed PlatformIO + Zephyr
generation path is not reliable when it receives that path directly. The
supported interface is therefore:

```powershell
.\nRF54L15\pio.ps1 build
.\nRF54L15\pio.ps1 clean
.\nRF54L15\pio.ps1 upload
.\nRF54L15\pio.ps1 monitor
```

`pio.ps1` exposes the project through a temporary no-space `SUBST` drive and
puts PlatformIO workspace/build/libdeps under
`%USERPROFILE%\.platformio-workspaces\nrf54l15`. Build, maintenance and test
actions must use this wrapper; a direct `pio run -d ".\nRF54L15"` command is
not the normal workflow. The automatic `test` action is a software-only
boundary. PlatformIO's `--without-uploading` flag prevents upload but does not
by itself prove a suite is hardware-free, so future physical target/HIL suites
must be kept outside the automatic `Tests: All (no flash)` selection.

The local baseline was verified on 2026-09-14 with PlatformIO Core 6.1.19,
SeeedStudio platform 1.0.0 at `1ec1287f8e4bc4067a6fd593991e36875aef989f`,
`framework-zephyr-nrf54l15` package `3.40201.251021` and Zephyr 4.2.1. The
platform URL in `nRF54L15/platformio.ini` is not commit-pinned, so board,
framework, Channel Sounding and driver support must be rechecked after a
package refresh.

### Planned physical topology

The target system contains one nRF54L15 in the main enclosure and one nRF54L15
for each connected remote geophone. Four remote geophones therefore mean five
nRF54L15 boards:

```text
ESP32-S3
|
+-- shared upstream I2C
|     |
|     +-- main-enclosure nRF54L15 (always visible)
|     |
|     +-- TCA9548A
|           |
|           +-- selected channel -> remote nRF54L15 + remote sensors
|           +-- another channel  -> remote nRF54L15 + remote sensors
|           +-- ...
|
+-- existing diploma hardware
```

With all TCA9548A downstream channels disabled, the nRF target still visible
on the upstream bus is conceptually the main-enclosure node. Selecting a
channel makes the corresponding remote target visible; deselecting it removes
that remote from the visible ESP32 topology. The main target remains visible
while channels are switched.

This is a future discovery concept, not an implemented I2C protocol. The
possible collision caused by identical nRF target addresses is an explicit
architecture gate. Address assignment, identity provisioning and collision
resolution must be selected and validated before I2C/TWIS implementation; no
address or workaround is invented here.

ESP32-S3 remains the main controller and the owner of the shared upstream
wired-I2C topology. The nRF54L15 is a target/peripheral on that upstream bus.
Each remote nRF54L15 is additionally the controller/master of a private local
sensor bus. The remote GY-BMI160 and BME280 are local devices behind their
remote nRF54L15; they are not devices directly visible on the ESP32/TCA9548A
bus. ESP32 reads their future nRF node snapshot instead. This planned split
does not change the current `sensor_bus` ownership or the existing INA3221
decision above.

The upstream nRF target address, identity provisioning and the possible
collision between simultaneously visible nRF targets remain unresolved
architecture gates. No address-allocation mechanism is invented here.

### Bluetooth geometry role

The nRF54L15 wireless purpose is spatial arrangement of remote geophones using
Bluetooth 6 Channel Sounding, not an RSSI-only substitute when Channel
Sounding is available. The nodes are expected to provide pairwise range data
for relative geometry reconstruction and later localization/3D-WebXR sensor
binding.

The main-enclosure node is not a permanently privileged Bluetooth master.
During a complete measurement cycle every connected nRF54L15 must receive an
initiator turn, with initiator/reflector roles switched at runtime as required
by the session. The future design must define scheduling, pairwise
measurements, quality/confidence, repeated-measurement aggregation,
missing/timeout/outlier handling and a distance matrix for the currently
available nodes. The geometry algorithm and solver contract remain TBD; do
not infer a mathematical algorithm from this architecture statement.

### External sensors, local bus, power and remote LED contract

The project uses the **plain Seeed Studio XIAO nRF54L15, not the XIAO nRF54L15
Sense**. Seeed documents the plain board as having no built-in sensor. Remote
geophone verticality therefore must not depend on the Sense-only
`LSM6DS3TR-C` or `DT_ALIAS(imu0)` device.

Each remote geophone node uses an external **GY-BMI160** 6-axis IMU controlled
locally by its nRF54L15. The main-enclosure nRF54L15 has no geophone whose
verticality must be measured and does not require this external IMU.

The XIAO exposes two I2C pin pairs. The project reserves them as follows:

| XIAO pin | nRF54L15 pin | Project use | Connected hardware |
| --- | --- | --- | --- |
| `D4 / SDA-0` | `P1.10` | shared upstream/TCA I2C SDA; nRF target/peripheral role is a future configuration gate | ESP32-S3/TCA9548A harness |
| `D5 / SCL-0` | `P1.11` | shared upstream/TCA I2C SCL; nRF target/peripheral role is a future configuration gate | ESP32-S3/TCA9548A harness |
| `D11 / SCL-1` | `P0.03` | private local sensor-bus SCL; nRF controller/TWIM role candidate | GY-BMI160 + BME280 on remote nodes |
| `D12 / SDA-1` | `P0.04` | private local sensor-bus SDA; nRF controller/TWIM role candidate | GY-BMI160 + BME280 on remote nodes |

The accepted remote harness power contract is:

```text
RJ45 +5V       -> XIAO 5V/VBUS
RJ45 GND       -> XIAO GND
RJ45 pin 2 +3V3 -> RESERVED/NC at the remote sensor assembly

XIAO 3V3_OUT   -> GY-BMI160 3V3/VDD/VDDIO as applicable
XIAO 3V3_OUT   -> BME280 3.3V/VCC/VDD/VDDIO as applicable
XIAO GND       -> GY-BMI160 GND and BME280 GND
```

`3V3_OUT` is the XIAO board's regulated power output, not a firmware GPIO.
The external sensor load, startup/transient margin and measured rail behavior
must still be validated; the regulator's component rating is not an approved
board-level external-load budget. The external RJ45 3.3 V conductor and
`3V3_OUT` must not be connected together because they are separate potential
power sources.

The selected GY-BMI160 breakout assembly is planned as follows: its module
`SCL`/`SDA` connect to local D11/D12, `3V3` connects to XIAO `3V3_OUT`, and
`VIN` remains unconnected. `CS/CSB` is strapped to 3.3 V for I2C and
`SA0/SDO` to GND for the BMI160 default address `0x68`. `INT1`, `INT2` and the
module's auxiliary `SCX`/`SDX`/`OCS` connections remain NC for the initial
assembly. These connector names and NC decisions are specific to the selected
breakout and must be checked against its silkscreen/datasheet before soldering.
Bosch documents the BMI160 primary interface as I2C or SPI and the `CSB`/`SDO`
straps; Zephyr provides `bosch,bmi160` bindings for both I2C and SPI.

The BME280 is on the same private D11/D12 bus and is read by the remote
nRF54L15 as part of the sensor-node aggregator. It is not connected directly
to the ESP32/TCA9548A bus and is not powered by an nRF GPIO. The BME280 I2C
address remains a module-level TBD: Bosch specifies `0x76` when SDO is low
and `0x77` when SDO is high, while the selected breakout's strap and pull-ups
must be verified. At the chip level, `CSB` must select I2C and `SDO` must not be
left floating; the selected breakout determines how those pins are exposed.
If a measurement history/window is required, the nRF must store it in RAM; the
BME280 itself exposes current/last conversion data rather than a project
history buffer. The future upstream snapshot may aggregate
`node_id`, timestamp, tilt, acceleration, gyro data, pressure, temperature,
humidity, Channel Sounding status and warning/error flags; its schema and
retention policy remain future protocol work.

The future dual-interface implementation must use different peripheral
instances: local D11/D12 as the nRF controller/TWIM side and upstream D4/D5 as
the nRF target/TWIS side. Zephyr documents that TWIS and TWIM share resources
and cannot occupy the same peripheral instance simultaneously. The installed
Seeed board description currently exposes its relevant `i2c22`/`i2c30` nodes as
TWIM by default, so the exact upstream TWIS overlay, instance mapping and
driver contract are implementation gates rather than current firmware facts.
The official Zephyr TWIS binding also notes that the standard Zephyr I2C slave
API is not available for these devices and points to a HAL-based workaround;
the upstream target protocol therefore requires an explicit API/maintenance
decision before implementation.

For static verticality, the accelerometer gravity vector is the primary
measurement: it gives the direction of gravity while the geophone is still.
The gyroscope measures angular velocity and is useful for motion detection or
future sensor fusion, but it is not by itself a stable static tilt reference.
The approximately 10-degree mechanical norm is a project reference, not an
approved firmware threshold. Threshold, warning zone, filtering, hysteresis,
debounce and calibration remain implementation parameters/TBD.

Each remote nRF54L15 has four external LEDs; the main-enclosure nRF54L15 has
none:

| nRF54L15 pin | XIAO pin | LED | Meaning |
| --- | --- | --- | --- |
| `P1.04` | `D0` | BLUE | verticality indication |
| `P1.05` | `D1` | GREEN | normal logger state |
| `P1.06` | `D2` | YELLOW | latched warning |
| `P1.07` | `D3` | RED | latched error |

The current nRF bring-up's `led0` alias is the onboard user LED and is not this
external remote assembly. Common firmware must tolerate absent external LEDs
and absent GY-BMI160 hardware on the main node and must not require either for
main-node boot or communication.

BLUE is intended to be steady within the accepted vertical state and to blink
more rapidly as filtered tilt error grows outside it, with a future-defined
saturated maximum rate and IMU-error behavior. No blink frequency or angle
transition is approved here.

The ESP32 `status_indicator` state machine consumes explicit application
status decisions. WARNING and ERROR are sticky until power cycle, and a later
NORMAL/CLEAR_NON_STICKY event does not clear them. Its effective state has
priority `ERROR > WARNING > NORMAL`; the present ESP32 board has no connected
status LED GPIOs, so this is a semantic contract rather than physical LED
proof. The future remote logger must preserve that user-facing persistence.
When WARNING and ERROR coexist, whether the remote shows red-only priority or
yellow+red simultaneously requires explicit confirmation before implementation.

The complete nRF test package, including the one-board/multi-node matrix and
the separation between native evidence and physical HIL, is in
[`docs/nrf54l15-test-task.md`](docs/nrf54l15-test-task.md).

## Hardware bus ownership

### I2C0 - INA3221 power telemetry

```text
GPIO9  SDA <-> INA3221 SDA
GPIO10 SCL  -> INA3221 SCL
address       0x40 (A0 tied to GND)
```

`sensor_bus` is the sole application owner of I2C0 and configures the shared bus at 100 kHz through `espressif/i2c_bus`. `ina3221_monitor` borrows the INA3221 device handle at address `0x40`; it never creates or deletes the bus. INA3221 runs continuous shunt+bus conversion on all three channels with hardware averaging x16 and 1.1 ms conversion time. A low-priority Core 0 task publishes one status update per second. Missing/faulty INA3221 hardware invalidates power telemetry and is retried without rebooting the device. The TCA9548A pod topology remains a documented watchlist until its address/channel/electrical contract is measured.

### Application events and input ownership

`components/app_events` owns one ESP-IDF `esp_event` user loop (`APP_EVENT`) with
queue 24, Core 0 affinity, priority 6 and a 6144-byte stack. It is not the
default system event loop. Wi-Fi, HTTPS onboarding, Web setup and input
publishers post the existing `int32_t` event payloads with zero wait ticks.

The five KS0466 ladder buttons use the official Button core/state machine with
keypad-owned `button_driver_t` adapters. Each timer callback reads the raw
ladder through the non-blocking `adc_service_try_read_average_raw()` broker
API; a busy ADC1 broker retains the previous decoded key state, and the cache
timestamp is taken after a completed read. The ADC1 handle remains private to
`adc_service`, so the vendor ADC driver cannot interleave a direct conversion
between discard and useful samples. Their measured raw boundaries remain in
`keypad_button_adapter.c`. The GPIO21 power button is an official GPIO Button
object with internal pulls disabled and a configured `long_press_time` of
150 ms after the vendor debounce state. The configured value is not a raw
GPIO21-to-latch timing claim; that interval is an explicit HIL measurement.
The power callback latches the shutdown EventGroup bit before its best-effort
APP_EVENT notification, so an event-queue-full condition cannot prevent safe
shutdown. Neither input path creates an application-owned polling task.

UI APP_EVENT delivery is a subscriber plus a bounded pending queue. Its user-loop
handler only copies event data and wakes the existing LVGL task; LVGL mutation
and the UI heartbeat happen in the existing LVGL timer callback. It therefore
does not wait for the LVGL, SD, TLS or Wi-Fi lifecycle locks.

`status_indicator` subscribes to APP_EVENT status decisions only. Green NORMAL,
yellow WARNING and red ERROR are represented by a priority state machine;
WARNING and ERROR are sticky until power cycle. The current board has no
connected LED GPIOs, so the official `led_indicator` backend remains disabled
and no GPIO is invented. `insights_bridge` is optional and disabled in the
baseline configuration; when enabled it exports selected runtime health
metrics only. Its cross-context initialized/retry state uses C11 atomic
publication, while connection events only notify the dedicated retry worker.

`seismic_dsp` is a synchronous, task-free foundation for measured
DC-removal/detrend, windowing, FIR/IIR band-pass/notch, FFT,
cross-correlation, normalization and RMS stages. It currently reports
placeholders and passes input through. Filter frequencies, coefficients and
localization matrix changes remain prohibited until AD7606 + ZF-24HP data is
available.

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

The current source sets the SD runtime ceiling to `SDMMC_FREQ_DEFAULT` (20 MHz), after 400 kHz discovery. This replaces the older 2 MHz setting in this document; physical stability on the current assembly still requires a separate hardware check.

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
Button adapters / shutdown latch
  -> INA3221 monitor
  -> sensor_bus / I2C0 release
  -> measurement service shutdown
  -> recorder stop / drain / finalize (LVGL display guard still alive)
  -> APP_EVENT UI subscriber / LVGL
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
