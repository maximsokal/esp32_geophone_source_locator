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
       -> seismic_buffer_init
       -> seismic_acquisition_init (driver/presence bring-up only, no sampling)
       -> adc_service_init
       -> keypad_init
       -> SPI2 init
       -> isolated SD startup check
       -> display hardware init
       -> LVGL boot UI
       -> storage_manager init
       -> ina3221_monitor_start (I2C0; non-fatal if hardware is absent)
       -> object recovery
       -> object store
       -> object service
       -> storage service
        -> Wi-Fi manager init
        -> interactive LVGL UI + APP_EVENT subscriber
        -> HTTP server
        -> Wi-Fi manager start
  -> nrf_topology_service_start (after successful app_startup_run)
  -> wait for shutdown request
  -> app_shutdown_run
```

Each major `app_startup_run()` stage is timed and logged so slow storage or hardware bring-up is visible in the serial log. `nrf_topology_service_start()` intentionally runs only after that interactive startup path has succeeded, so low-rate nRF discovery/reconciliation does not extend the timed boot sequence.

The shared I2C0 sensor bus (`ina3221_monitor_start`, and later the
TCA9548A/nRF54L15 upstream topology) deliberately opens only after the
keypad, display and SD card stack are already interactive, not as the first
hardware brought up. A main-enclosure XIAO nRF54L15 is wired as an upstream
I2C target on this same bus; see `docs/i2c-boot-race-conditions.md` for the
full boot-ordering/race-condition audit, including the still-open questions
around that node's power/reset sequencing.

AD7606 acquisition does not auto-start at boot. `seismic_acquisition_init()`
only brings up the driver/presence detection; active sampling
(`seismic_acquisition_start()`) starts on demand — from the local UI
entering the Measurements screen, or from an explicit Web `acquisition_start`
/ `recording_start` request — so the ADC is never running with nothing
consuming its samples. See `docs/i2c-boot-race-conditions.md` item 5.

## Core ownership

The project deliberately separates service work from acquisition work.

### Core 0 - services

- LVGL handler task (`esp_lvgl_port`)
- ESP-IDF APP_EVENT user-loop task
- official Button component service for keypad and power-button sampling
- INA3221 low-rate power monitor
- low-priority `nrf_topology` discovery/sensor transport service
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
| nRF topology / sensor transport | 0 | 1 | 4096 |

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
| **GPIO41** | output | AD7606 `CS` **plus external 10 kOhm pull-up to AD7606 VIO (LD1117 OUT, 3.3 V)** | `CS` is active LOW. The 10 kOhm resistor keeps the ADC deselected while ESP32 is resetting/booting and GPIO41 is not yet actively driven. Firmware then drives CS HIGH before handing the pin to SPI3. The pull-up terminates at the AD7606's own VIO, never at the always-on ESP32 3.3 V (see the AD7606 VIO section). **Do not remove this pull-up when rewiring.** |
| **GPIO42** | output | AD7606 `CVA` + `CVB`, physically tied together | One rising edge starts the simultaneous conversion of all eight AD7606 channels. Both CONVST inputs must remain tied to this GPIO for simultaneous sampling. |
| **GPIO43** | output | ESP32-S3 UART0 TX -> external CH340 `RXD` for passive serial monitoring | Used for battery-only COM6 logging. In passive-monitor setup only ESP32 TX -> CH340 RXD and common GND are used. |
| **GPIO44** | input / reserved | UART0 RX; **not connected to CH340 TXD in passive-monitor setup** | Leave disconnected during passive COM6 monitoring so the CH340 cannot drive the ESP32. |
| **GPIO47** | input | AD7606 `BUSY` | Conversion-status input. Firmware waits for BUSY release before SPI readout. |

All other ESP32-S3 GPIOs are currently **unallocated by `board_config.h`** unless another documented bench experiment explicitly says otherwise. Treat that as "free in the current firmware", not as permission to move one of the connections above.

### Physical resistor / temporary analog wiring notes

#### GPIO41 AD7606 CS pull-up - permanent prototype requirement

```text
AD7606 VIO (LD1117V33 OUT, 3.3 V)
  |
 [10 kOhm]
  |
  +------ GPIO41 ------ AD7606 CS
```

Since 2026-09-23 the pull-up is fed from the AD7606's own VIO rail, not from
ESP32 3.3 V: when the AD7606 +5 V rail is off, VIO and the pull-up are off
too, so there is no ESP32 3.3 V -> 10 kOhm -> CS path into an unpowered
AD7606 (digital inputs must stay <= VDRIVE + 0.3 V).

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
- The GPIO41 pull-up terminates at the **AD7606 VIO** rail (LD1117V33 OUT), not at ESP32 3.3 V (changed 2026-09-23).
- **AD7606 VIO/VDRIVE is powered by its own LD1117V33** (TO-220, pins GND/OUT/IN; the tab is VOUT) fed from the **same +5 V that powers AD7606 AVCC**, with 100 nF IN->GND and 10 uF (electrolytic) OUT->GND. The former ESP32 3V3 -> AD7606 VIO wire is **removed**; two 3.3 V sources must never be tied together. This makes AVCC = 0 V with VDRIVE = 3.3 V (an AD7606 absolute-maximum violation: VDRIVE <= AVCC + 0.3 V) impossible in hardware. Post-rework check (before connecting ESP32): +5 V off -> VIO ~= 0 V; +5 V ~= 5 V -> VIO ~= 3.3 V; no VIO<->GND short; no VIO<->ESP32 3V3 connection.
- **MAIN XIAO nRF54L15** is powered from **ESP32 5 V -> XIAO 5V/VBUS** with common GND (changed 2026-09-23), so it is powered whenever the ESP32 is and is never left unpowered on the active upstream I2C bus (MAX-108 scenario). Do not connect XIAO USB while it is fed from ESP32 5 V without checking the XIAO's VBUS path.
- The CH340 VCC pin stays disconnected in passive battery-monitor mode; only RXD and GND are connected.
- Exact present VCC rail routing for every external module is not represented by firmware GPIO definitions. Before a complete prototype teardown, add the measured/verified module power wiring here rather than reconstructing it from memory.

## nRF54L15 subproject and distributed sensor topology

`nRF54L15/` is an independent nRF application subtree inside this repository.
The repository root remains the ESP-IDF ESP32-S3 project; the WebUI and nRF
application remain separate build surfaces and the ESP-IDF root build does not
depend on the nRF build.

The **canonical nRF54L15 production/flash toolchain is nRF Connect SDK v3.4.0
(Zephyr 4.4.0)** using the upstream board target
`xiao_nrf54l15/nrf54l15/cpuapp`. All nRF54L15 boards use the same `src`,
`include` and `zephyr` source/configuration tree and one common runtime firmware
image. MAIN/REMOTE topology and future Channel Sounding initiator/reflector
roles are runtime state; there are no compile-time `central`, `remote`,
`initiator` or `reflector` production variants.

The historical Seeed PlatformIO environment `seeed-xiao-nrf54l15` remains
commit-pinned to platform revision
`1ec1287f8e4bc4067a6fd593991e36875aef989f` / Zephyr 4.2.1 only as an explicit
**compatibility smoke build**. It is not the authoritative production artifact
and `pio.ps1 upload` is not the official flash path.

### Windows build boundary

The checkout path contains spaces, so the supported production interface is the
pinned NCS wrapper:

```powershell
.\nRF54L15\ncs.ps1 build
.\nRF54L15\ncs.ps1 clean
.\nRF54L15\ncs.ps1 flash
.\nRF54L15\ncs.ps1 monitor -Port COM7
```

`ncs.ps1` is pinned to NCS v3.4.0. When called from an ordinary PowerShell
without `west`, it relaunches itself through
`nrfutil sdk-manager toolchain launch --ncs-version v3.4.0`. On Windows it
exposes the source through a temporary no-space `SUBST` drive and stores NCS
build output under
`%USERPROFILE%\.ncs-workspaces\esp32_geophone_source_locator`.

Canonical `ncs.ps1 build` uses `nRF54L15/zephyr/prj.conf` only. The additive
`nRF54L15/ncs/channel_sounding.conf` remains the historical MAX-45
SoftDevice/Channel-Sounding proof fixture until MAX-51 promotes the reviewed
RAS/CS configuration into production. Reproducible proof actions are
`ncs.ps1 cs-proof`, `compare` and `probe`; none of them is a second production
firmware variant. `flash` is an explicit hardware action and is excluded from
`Tests: All (no flash)`.

PlatformIO commands may still be used for explicitly named compatibility
checks through `pio.ps1`, but they must not be treated as production build,
flash, Channel Sounding controller or HIL evidence.

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

The wired bootstrap/discovery protocol is implemented. Every nRF reset starts
UNASSIGNED at discovery address `0x30`; a validated ASSIGN moves the
main-enclosure node to `0x31` or a selected REMOTE to `0x32`. The ESP32
`nrf_topology_service` performs MAIN-first reconciliation, keeps downstream TCA
channels closed while reconciling the direct bus, and scans REMOTE channels
one-hot. Warm-reset reconciliation accepts correctly assigned nodes without
reassigning them, REMOTE node IDs remain stable as `channel + 1`, and
collisions, malformed identities and moved/stale assignments are surfaced as
conflict/error state rather than silently accepted. The byte-level bootstrap
contract is documented in
[`docs/nrf-upstream-bootstrap-protocol.md`](docs/nrf-upstream-bootstrap-protocol.md),
and the ESP32 orchestrator contract is in
[`docs/nrf-topology-service.md`](docs/nrf-topology-service.md).

Every XIAO runs the WDT31 hardware watchdog (MAX-244, 8 s timeout). The main
loop feeds it only while the sensor worker and the system workqueue both
checked in within the last 5 s (`nrf_watchdog_health`). The sensor worker
therefore never blocks longer than 1 s, even when idle. A hung node resets
and comes back UNASSIGNED at the discovery address, where the ESP32 topology
service re-assigns it. The reset cause is printed on the XIAO console at
boot; it is not yet reported over I2C, because the status decoder rejects
unknown flag bits, so that needs a protocol version change.

ESP32-S3 remains the wired-I2C controller. `sensor_bus` is still the sole
application owner of I2C0; `nrf_topology_service` borrows that bus and the
TCA9548A handle and is the sole application-level owner of TCA channel
selection. The nRF54L15 is a target/peripheral on the upstream bus, while each
REMOTE is also the controller/master of its private `i2c30` sensor bus. The
GY-BMI160 and BME280 are therefore never directly visible on the ESP32/TCA
bus. A successful REMOTE assignment activates the dedicated low-priority nRF
sensor worker using copied assignment metadata; MAIN and UNASSIGNED nodes do
not perform private sensor I/O. REMOTE activation prepares the optional IMU and
environment sources independently and produces an initial aggregate sample
when at least one source is available.

`nrf_upstream_target_zephyr` remains the platform lifecycle/re-addressing
adapter, while `nrf_upstream_protocol` and the bootstrap runtime define the
project addresses and wire semantics. The upstream TWIS DMA buffer is
explicitly configured to 128 bytes. REMOTE target reads expose a fixed
112-byte sensor frame whose bytes `0..27` are the exact legacy STATUS frame;
the extension carries sensor request ID, sequence, validity/fault masks, nRF
sample-complete uptime, IMU/environment payload and a full-frame CRC. Explicit
sensor refresh uses the fixed 12-byte SENSOR_REFRESH request. Physical proof
that a controller can STOP after only the 28-byte prefix of a prepared
112-byte TWIS response and then start the next transaction cleanly remains a
HIL gate; software tests do not claim that electrical/driver behavior.

The same ESP32 `nrf_topology` Core-0 task also consumes those REMOTE snapshots;
there is no second TCA/sensor task. Sensor-cache identity includes hardware ID,
node ID, mux channel, runtime address and bootstrap `STATUS.request_id` as an
assignment epoch, so a reboot/reassignment invalidates old samples even on the
same physical slot. A bounded transaction performs verified one-hot select,
optional MAIN identity validation, REMOTE identity validation, SENSOR_REFRESH,
112-byte polling and verified close with a 500 ms total deadline and 10 ms
poll interval. Public callers can only request refreshes and read race-safe
cached results; raw I2C/TCA handles remain private. A new/reassigned REMOTE
queues one initial ESP-side refresh, but there is no continuous automatic
sensor polling. nRF completion uptime and the separate ESP32 receipt uptime are
freshness/debug metadata in different clock domains and are not geophone TDOA
timestamps. The consumer contract is documented in
[`docs/nrf-topology-sensor-consumer.md`](docs/nrf-topology-sensor-consumer.md).

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
| `D4 / SDA-0` | `P1.10` | shared upstream/TCA I2C SDA; `i2c22` nRF target/TWIS foundation | ESP32-S3/TCA9548A harness |
| `D5 / SCL-0` | `P1.11` | shared upstream/TCA I2C SCL; `i2c22` nRF target/TWIS foundation | ESP32-S3/TCA9548A harness |
| `D11 / SCL-1` | `P0.03` | private local sensor-bus SCL; `i2c30` nRF controller/TWIM | GY-BMI160 + BME280 on remote nodes |
| `D12 / SDA-1` | `P0.04` | private local sensor-bus SDA; `i2c30` nRF controller/TWIM | GY-BMI160 + BME280 on remote nodes |

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
history buffer. The implemented upstream REMOTE snapshot transports the
bootstrap identity/status prefix plus sensor request ID, sample sequence,
validity/fault masks, nRF completion uptime, acceleration, gyro, IMU
temperature, environment temperature, pressure and humidity. The REMOTE sensor
worker also contains a software-only orientation estimator that can consume
BMI160 samples without triggering a BME280 conversion. It does not change the
112-byte upstream frame and does not provide persistent history, synchronized
cross-node time, Channel Sounding geometry or physical external LED/logger
state.

The dual-interface foundation uses distinct peripheral instances: local D11/D12
remains `i2c30` in controller/TWIM mode and upstream D4/D5 uses `i2c22` as the
nRF target/TWIS side. The project overlay overrides the upstream XIAO `i2c22`
pinctrl to TWIS functions on P1.10/P1.11 and changes its compatible to
`nordic,nrf-twis`; it does not convert `i2c30`. In the pinned NCS v3.4.0 /
Zephyr 4.4.0 source, `drivers/i2c/i2c_nrfx_twis.c` implements the standard
buffer-mode Zephyr target API through `i2c_target_register()` and
`i2c_target_unregister()`. No project HAL bypass is required. The
`nrf_upstream_target_zephyr` adapter exposes that platform lifecycle,
re-address rollback and raw `hwinfo_get_device_id()` access. Project addresses
`0x30`/`0x31`/`0x32`, ASSIGN/STATUS framing, deferred role assignment and the
backward-compatible REMOTE sensor snapshot protocol are implemented above that
adapter. Bluetooth Channel Sounding, synchronized clocks, persistent sensor
history and the physical remote LED/logger backend remain separate future work,
while 28-byte early-STOP versus full 112-byte TWIS behavior remains a physical
HIL validation item.

For static verticality, the accelerometer gravity vector is the primary
measurement: it gives the direction of gravity while the geophone is still.
The gyroscope measures angular velocity and is used only as a motion/stability
gate, not as an accumulated absolute angle. The implemented pure orientation
estimator uses integer fixed-point gravity geometry, a 1/4 EMA, explicit
10-degree exit / 8-degree re-entry hysteresis defaults, a configurable gravity
window and graded OUTSIDE severity. Its signed reference axis is intentionally
`UNCONFIGURED` in production until the installed BMI160-to-geophone mechanical
axis is recorded and HIL-verified, so this software slice does not silently
assume that X/Y/Z is vertical. Physical LED polarity, resistor wiring, blink
rates and logger warning/error latch policy also remain HIL/configuration work.

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

The software estimator now exposes semantic BLUE states without driving the
external GPIO: `UNAVAILABLE`, `MOVING`, `ACCEPTED`, and `OUTSIDE` with
slow/medium/fast blink classes. `ACCEPTED` maps to steady BLUE later; graded
`OUTSIDE` classes map to progressively faster blinking. No physical D0 GPIO
polarity or blink frequency is approved here, and `MOVING` must not be rendered
as a false vertical indication.

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

`sensor_bus` is the sole application owner of I2C0 and configures the shared bus at 100 kHz through `espressif/i2c_bus`. `ina3221_monitor` borrows the INA3221 device handle at address `0x40`; it never creates or deletes the bus. INA3221 runs continuous shunt+bus conversion on all three channels with hardware averaging x16 and 1.1 ms conversion time. A low-priority Core 0 task publishes one status update per second. Missing/faulty INA3221 hardware invalidates power telemetry and is retried without rebooting the device. The same bus may also expose a single discovered TCA9548A at `0x70..0x77`; `sensor_bus` owns its device handle, while `nrf_topology_service` borrows it and exclusively performs verified one-hot channel selection/close for MAIN/REMOTE topology and sensor transactions. The nRF runtime addresses are discovery `0x30`, MAIN `0x31` and selected REMOTE `0x32`.

Battery states and low-battery shutdown (MAX-180/182/183): on top of the source classifier (USB / USB_CHARGING / BATTERY), `ina3221_power_ux` publishes one user-facing state: USB_ONLY_NO_BATTERY, USB_CHARGING, USB_BATTERY_READY (with a `battery_full` flag), BATTERY, BATTERY_LOW, BATTERY_CRITICAL or UNKNOWN. The LOW and CRITICAL bands use separate enter and exit voltages (hysteresis). A state change needs 3 consecutive samples, CRITICAL needs 5, and the first sample after boot is never an alarm. `ina3221_low_battery` reacts to it:
- BATTERY_LOW gives a persistent warning.
- BATTERY_CRITICAL starts a grace countdown (default 10 s) asking to connect USB. Stable USB power within it cancels the countdown.
- When the countdown ends, the monitor calls the handler the application registered. That handler calls `power_control_request_shutdown(POWER_SHUTDOWN_REASON_LOW_BATTERY)`.

The request sets the same latch as the power button, so the existing sequence runs unchanged: keypad and Web command ingress close, the recorder is finalised, SD is flushed, and the OFF pulse follows. Once requested, the shutdown is not cancelled. The TFT header shows a power badge on every screen (battery level, USB, charging, `LOW`, a red `USB 8s` countdown). The WebUI shows a banner, and `/api/status` exposes `power.state`, `low_battery` and `shutdown_in_s`. All voltages are Kconfig values (`Battery states and low-battery shutdown` menu), and the defaults are uncalibrated 1S source-voltage estimates. The CRITICAL threshold must leave room for the grace period plus the shutdown before the UPS board's own cutoff (MAX-184 HIL).

Battery percentage (MAX-181, `ina3221_soc`): the percentage comes from a hybrid estimator.
- **Anchoring.** It anchors from the voltage curve only when the pack is at rest (|I| < 50 mA), and at 100 % on a proven full charge (USB present, charge terminated).
- **Coulomb counting.** Between anchors it integrates the measured battery current (positive = discharge).
- **Drift correction.** After 30 min at rest it re-anchors from the voltage curve.
- **Gaps.** A gap longer than 5 s, or missing data, is never integrated as zero current: the estimate drops to `VOLTAGE_ONLY`.
- **Capacity.** `POWER_BATTERY_CAPACITY_MAH` defaults to 0 (unknown), because the pack topology is unverified, and with 0 the estimate stays `VOLTAGE_ONLY`.
- **Reboot.** Nothing is persisted: after a reboot the charge that flowed while off is unknown.

`/api/status` reports the source as `power.battery_soc_source`.

Over-current limits (MAX-245): the monitor runs a software over-current check on the sign-corrected current of all three channels. A level needs two consecutive samples (~2 s at 1 Hz); a warning clears below 90 % of its threshold, and a CRITICAL level stays latched until cleared. The thresholds are Kconfig values (`INA3221 over-current limits` menu). The defaults are engineering estimates, not measurements: battery 1.2/1.5 A, ESP32 5 V 0.8/1.2 A, AD7606 5 V 0.15/0.25 A, all below the 1.638 A shunt full scale. They must be calibrated on the bench. A latched critical on the AD7606 rail stops acquisition through the MAX-242 interlock (Hi-Z pins, `power_gate = "overcurrent"`), and the next explicit acquisition start clears the latch. The INA3221 CRI/WAR pins are not wired, so the chip's alert limits (0x07–0x0C) are observability only. Of them, only CH1 is programmed: the rail shunts are installed load→source, their load current reads negative, and a positive comparator limit could never trip, so CH2/CH3 comparators are disabled. This is slow software protection (seconds); a short needs the supply's own current limit or a fuse.

Stuck-bus handling (MAX-243, NXP UM10204 bus clear): before I2C0 is created, `sensor_bus` takes GPIO9/10 as open-drain GPIOs and bit-bangs a clear — SDA held low is released with up to 9 SCL clocks followed by a STOP; SCL held low is never clocked against and is only recorded (the holder is a target; only its reset/power cycle clears it). After a bus-level transfer error `nrf_topology_service` calls `sensor_bus_recover()`. A plain NACK is filtered out first: ESP-IDF 5.5 reports it as `ESP_ERR_INVALID_STATE` like a timeout, so `sensor_bus_classify_transfer_error()` runs an address probe and turns a NACK into `ESP_ERR_NOT_FOUND` (MAX-248). In `sensor_bus_recover()`: a line must read low on every sample over ~1 ms to count as stuck; a stuck SDA gets `i2c_master_bus_reset()`, after which the TCA9548A is closed and read back. Idle lines (a NACK) leave the mux alone. Attempts are rate-limited (1 s, doubling per consecutive failure, max 60 s); counters are available through `sensor_bus_get_recovery_status()`. A reduced clock for REMOTE channels is not implemented — the fix for slow edges on RJ45 cables is the pull-up sizing in MAX-214.

#### I2C pull-ups over the TCA9548A / RJ45 runs (MAX-214 - open HIL item)

Today the only pull-ups on the upstream GPIO9/GPIO10 bus are the ESP32-S3
internal ones (~45 kOhm typical); the nRF upstream TWIS pins deliberately have
none (MAX-116), and no pull-ups are documented on the TCA9548A downstream
channels. TI sizes each bus segment with

```text
Rp(max) = tr(max) / (0.8473 * Cb)        Standard-mode: tr(max) = 1 us
```

For an estimated Cb ~= 300 pF per 5 m RJ45 channel: 45 kOhm gives tr ~= 11.4 us
(out of spec), 4.7 kOhm ~= 1.19 us (still slightly out), Rp(max) ~= 3.9 kOhm.
Until measured, treat 2.2-3.3 kOhm to 3.3 V on EACH downstream channel as the
starting point (keep total sink current <= 3 mA), then verify tr and VOL at the
far end with an oscilloscope. NXP PCA9615 (differential I2C, full spec to 3 m)
or a lower bus clock is the fallback for 5 m runs. Pull-ups must never feed an
unpowered node (MAX-108/MAX-155). Record the measured Cb, chosen Rp and
measured tr here.

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
cross-correlation, normalization and RMS stages. Its parameter-free stages
(DC removal, energy RMS, peak normalization, Hann windowing) are implemented
and process each block in place; each is stateless across blocks, so an
acquisition gap cannot leave stale cross-block state behind. FIR, IIR,
band-pass, notch, FFT and cross-correlation remain unimplemented
placeholders and are reported as such in `placeholder_stages`. Filter
frequencies, coefficients and localization matrix changes remain prohibited
until AD7606 + ZF-24HP data is available.

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

GPIO13 is shared by the SD card MISO and the ILI9341 SDO, but no production
code ever reads the display (MAX-215). If the TFT module allows it, leave its
SDO unconnected so the two drivers can never contend on MISO. `spi_bus`
drives TFT_CS/SD_CS high early with internal pulls disabled; during
reset/boot before that, both CS lines are defined only if the modules carry
their own pull-ups -- if they do not, fit ~10 kOhm to 3.3 V on each (the SD
spec expects a CS pull-up). Record the module check here; do not attribute
SD faults to this without a measurement.

The current source sets the SD runtime ceiling to `SDMMC_FREQ_DEFAULT` (20 MHz), after 400 kHz discovery. This replaces the older 2 MHz setting in this document; physical stability on the current assembly still requires a separate hardware check.

### SPI3 - AD7606 only

```text
GPIO39 <- AD7606 DOUTA/DB7
GPIO40 -> AD7606 RD/SCLK
GPIO41 -> AD7606 CS        + 10 kOhm pull-up to AD7606 VIO (LD1117V33 OUT)
GPIO42 -> AD7606 CVA + CVB (tied together)
GPIO47 <- AD7606 BUSY
GPIO2  -> AD7606 RESET
```

SPI3 is read-only for this AD7606 connection; MOSI is intentionally unused.

**Power interlock (MAX-242, firmware layer on top of the LD1117V33 VIO fix).**
`ad7606_driver` loads the output levels (CS=1, RESET=0, CONVST=0) before it
enables the outputs, so CS never glitches LOW, and every exit path (deinit,
any init failure, rail loss) calls `ad7606_driver_enter_safe_state()`:
RESET/CONVST/CS/SCLK become Hi-Z inputs without internal pulls, with CS held
deselected by its pull-up to the AD7606 VIO. With
`CONFIG_SEISMIC_AD7606_RAIL_INTERLOCK` (default y) `ad7606_acquisition_start()`
refuses to start -- without touching any AD7606 pin -- when a fresh INA3221
CH3 reading shows the AD7606 +5 V below 4.75 V; while running, two consecutive
fresh readings below 4.50 V stop acquisition, release the pins and latch a
power fault (`acquisition.serial.power_fault_latched` in `/api/status`, shown
on the Sensors page) until the next explicit start. Missing/stale INA3221
data only warns. Disable the option only if the AD7606 supply is not routed
through INA3221 CH3.

**GPIO drive strength (MAX-247 review, decision: unchanged).** The outputs to
the AD7606, TFT, SD card and power switch stay at the ESP-IDF default drive
capability. `gpio_set_drive_capability()` only changes the output driver's
strength and edge rate; it is **not** a programmable current limit and would not
protect a pin from a short or from back-powering an unpowered device. That
protection comes from the hardware: separate supplies (LD1117V33 VIO), Hi-Z
safe states in firmware (MAX-242), and, if needed, series resistors (for
example 33–100 Ohm on the AD7606 SCLK/CS/CONVST lines) or buffers. Lowering the
drive strength is only worth revisiting if a bench measurement shows ringing
or EMI on the long AD7606/RJ45 lines.

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
  -> nRF topology / sensor transport service
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
