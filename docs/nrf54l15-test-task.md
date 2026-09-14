# Task: develop and run nRF54L15 tests

Status: **planning only**. This task prepares the future nRF54L15 firmware and
its verification work. It does not itself implement BLE, Bluetooth Channel
Sounding, I2C/TWIS, BMI160, GPIO or LED production drivers.

Related sources of truth:

- repository boundaries and physical topology: [`../ARCHITECTURE.md`](../ARCHITECTURE.md);
- master local validation flow: [`local-verification.md`](local-verification.md);
- evidence vocabulary: [`verification-evidence-matrix.md`](verification-evidence-matrix.md);
- existing ESP32 test layers: [`firmware-test-plan.md`](firmware-test-plan.md);
- nRF project entry point: [`../nRF54L15/README.md`](../nRF54L15/README.md).

## 1. Goal and non-goals

The future implementation/test work must make every installed XIAO nRF54L15
usable from the same production firmware image. It must eventually provide the
ESP32-S3 with node identity/topology, Bluetooth 6 Channel Sounding range data,
and remote-geophone orientation status while keeping `nRF54L15/` isolated from
the ESP-IDF root project.

This task does not authorize:

- separate `central` and `remote` production firmware variants;
- separate production PlatformIO environments for node roles;
- an invented I2C address-allocation scheme before it is reviewed;
- hard-coded tilt/blink thresholds that have not been approved/measured;
- treating software/native tests as physical HIL evidence;
- changing ESP32/WebUI production behavior merely to make nRF tests easier.

## 2. Current verified baseline

The repository contains the independent nRF bring-up project:

```text
nRF54L15/
  platformio.ini
  pio.ps1
  src/main.c
  test/
  zephyr/CMakeLists.txt
  zephyr/prj.conf
```

The current production environment is:

```ini
[env:seeed-xiao-nrf54l15]
platform = https://github.com/Seeed-Studio/platform-seeedboards.git
framework = zephyr
board = seeed-xiao-nrf54l15
```

The current `src/main.c` is only a temporary board/toolchain blink through the
onboard `led0` alias. It is not the future external remote LED implementation
and proves no Channel Sounding, topology or IMU functionality.

The supported Windows entry point is `nRF54L15/pio.ps1`. The checkout path
contains spaces, so the wrapper exposes the project through a temporary
no-space `SUBST` drive and places generated PlatformIO/Zephyr data under:

```text
%USERPROFILE%\.platformio-workspaces\nrf54l15
```

Supported commands are:

```powershell
.\nRF54L15\pio.ps1 build
.\nRF54L15\pio.ps1 clean
.\nRF54L15\pio.ps1 upload
.\nRF54L15\pio.ps1 monitor
.\nRF54L15\pio.ps1 test
```

A direct `pio run -d ".\nRF54L15"` command is not the normal workflow.

The baseline wrapper build was verified on 2026-09-14 with PlatformIO Core
6.1.19, SeeedStudio platform 1.0.0 at
`1ec1287f8e4bc4067a6fd593991e36875aef989f`, framework package
`3.40201.251021`, and Zephyr 4.2.1. The platform URL is not commit-pinned, so
board/framework/Channel-Sounding/BMI160 support must be rechecked after a
package refresh.

## 3. Production architecture contract

### One production firmware for every node

All installed XIAO nRF54L15 boards use:

- one production source tree;
- one production firmware implementation;
- one production firmware binary/configuration shape;
- runtime node identity/topology/ranging roles.

There must be no compile-time `central`/`remote` firmware split. A future
host/native-only test environment is allowed if required for software testing;
it must never become a second production node-role image.

### Wired topology

The planned topology is:

```text
ESP32-S3
|
+-- shared upstream I2C
|     |
|     +-- main-enclosure nRF54L15 (always visible)
|     |
|     +-- TCA9548A
|           |
|           +-- selected channel -> remote nRF54L15 + remote BME280
|           +-- another channel  -> remote nRF54L15 + remote BME280
|           +-- ...
|
+-- existing diploma hardware
```

There is one main-enclosure nRF54L15 and one remote nRF54L15 per connected
remote geophone. Four remote geophones therefore mean five nRF54L15 boards.

The intended discovery concept is:

1. disable all TCA9548A downstream channels;
2. identify the nRF target still visible upstream as the main-enclosure node;
3. select TCA channels one at a time;
4. associate the newly visible remote target with that channel.

This is a topology concept, not an implemented protocol. Multiple visible nRF
nodes may otherwise share the same target address. Address assignment,
identity provisioning and collision resolution are explicit architecture
gates and must be resolved before I2C/TWIS implementation.

ESP32-S3 remains the wired-I2C controller. A remote BME280 has independent
3.3 V power. It is not powered by the nRF54L15 and the nRF54L15 is not assumed
to be the BME280 I2C master.

### Bluetooth 6 Channel Sounding

The target ranging technology is Bluetooth 6 Channel Sounding, not an RSSI-only
replacement when Channel Sounding is available.

The main-enclosure nRF54L15 is not a permanently privileged Bluetooth master.
During a complete measurement cycle every connected nRF54L15 must receive an
initiator turn. Future implementation/tests must cover:

- Channel Sounding session scheduling;
- runtime initiator/reflector role switching;
- pairwise ranging;
- repeated-measurement aggregation;
- quality/confidence;
- timeout/missing/outlier handling;
- distance-matrix construction for currently available nodes;
- reconnect/reboot/partial-topology behavior;
- node count independent of the reference four-remote layout.

The exact geometry solver and acceptance thresholds remain separate design
work.

## 4. Actual remote orientation hardware: plain XIAO + external GY-BMI160

The installed target is the **plain Seeed Studio XIAO nRF54L15, not the XIAO
nRF54L15 Sense**. The plain board has no built-in IMU. Therefore tests and
future firmware must not rely on the Sense-only `LSM6DS3TR-C`, `imu0`, or
Sense built-in-sensor examples as if that device existed on the installed
hardware.

Each remote geophone node uses an external **GY-BMI160** 6-axis IMU controlled
by its local nRF54L15. The main-enclosure nRF54L15 does not have a geophone
whose verticality must be measured and therefore does not require this
external IMU.

### Local IMU bus allocation

The XIAO exposes two I2C pin pairs. The project reserves them as follows:

| XIAO pin | nRF54L15 pin | Project use |
| --- | --- | --- |
| `D4 / SDA-0` | `P1.10` | shared upstream/TCA I2C SDA to ESP32 |
| `D5 / SCL-0` | `P1.11` | shared upstream/TCA I2C SCL to ESP32 |
| `D11 / SCL-1` | `P0.03` | local GY-BMI160 I2C SCL |
| `D12 / SDA-1` | `P0.04` | local GY-BMI160 I2C SDA |

The GY-BMI160 is powered from the remote 3.3 V/GND rails. BMI160 supports I2C
or SPI on its primary interface, but this project chooses the XIAO's second
I2C pair so the IMU does not consume D8/D9/D10 plus a chip-select pin and does
not share the ESP32/TCA upstream I2C pair.

Zephyr provides `bosch,bmi160` bindings for both I2C and SPI. Before production
implementation, verify against the actual GY-BMI160 module:

- module I2C address/selection strap;
- whether SDA/SCL pull-ups are already populated on the module;
- required supply pin and 3.3 V behavior;
- whether INT1/INT2 are needed; if not needed initially, do not consume extra
  XIAO pins merely because the module exposes them;
- driver/Kconfig behavior in the exact Seeed/Zephyr package used by this
  project.

For stationary verticality the accelerometer gravity vector is the primary
absolute reference. Gyroscope data is useful for motion detection and optional
sensor fusion, but gyro integration alone is not a stable static tilt
reference.

The project's mechanical reference remains approximately 10 degrees maximum
tilt. That is not yet a single hard-coded firmware threshold. Filtering,
calibration, threshold, warning band, hysteresis/debounce and invalid-sensor
behavior must be explicit parameters/requirements before implementation.

## 5. Remote LEDs

Each remote node has four external LEDs:

| nRF54L15 pin | XIAO pin | LED | Planned meaning |
| --- | --- | --- | --- |
| `P1.04` | `D0` | BLUE | verticality/tilt indication |
| `P1.05` | `D1` | GREEN | normal logger state |
| `P1.06` | `D2` | YELLOW | latched warning |
| `P1.07` | `D3` | RED | latched error |

The main-enclosure nRF54L15 has none of these four external LEDs. Common
firmware must boot and communicate when both the external LED assembly and the
GY-BMI160 are absent on the main node.

BLUE is independent of the logger traffic-light state. Intended UX:

- accepted vertical state -> BLUE steady;
- tilt outside the accepted state -> BLUE blinks;
- larger filtered tilt error -> faster blink;
- maximum blink rate, thresholds, hysteresis and IMU-failure indication are
  future measured/configured parameters, not invented constants.

Logger LEDs follow the user-facing semantics of the ESP32 status state:

- normal -> GREEN steady;
- WARNING -> YELLOW latched until reset/power cycle;
- ERROR/FATAL -> RED latched until reset/power cycle;
- a later success/normal event does not clear a warning/error latch.

The existing ESP32 effective state uses `ERROR > WARNING > NORMAL`, but the
remote has independent yellow/red outputs. Red-only priority versus simultaneous
yellow+red when both latches are set remains an explicit UX decision that must
be confirmed before implementation/tests freeze the expected physical output.

## 6. Test levels

### A. Build/configuration tests

Prove at minimum:

1. `nRF54L15/` remains an independent PlatformIO project;
2. repository root remains ESP-IDF, not PlatformIO;
3. there is one production nRF firmware/environment and no role-specific
   central/remote production variants;
4. `.\nRF54L15\pio.ps1 clean` then `build` succeeds from the real checkout path
   containing spaces;
5. an unchanged second build succeeds incrementally;
6. generated PlatformIO/Zephyr data stays outside the repository;
7. nRF build is independent from ESP-IDF and WebUI build;
8. toolchain/platform/framework versions are visible so drift can be detected.

### B. Software unit/native tests

Pure policy/state logic should be deterministic and hardware-independent where
possible. Cover:

| Area | Required cases |
| --- | --- |
| Node identity/state | boot, unassigned, main, remote, unavailable, reconnect, reset, invalid transition |
| Runtime topology | main visible with mux closed; selected remote appears; disappearing/reappearing channel; 0..N remotes; duplicate/ambiguous identity rejected |
| Logger latches | normal, warning latch, error latch, later success does not clear, approved simultaneous warning/error UX |
| Tilt/BLUE policy | vertical, threshold boundaries, monotonic blink-rate mapping, maximum rate, invalid IMU, no chatter near boundary |
| Filtering/hysteresis | startup fill, noisy samples, stationary gravity, motion entry/exit, timeout/reset |
| BMI160 policy | sample-valid/invalid/timeout state handling without pretending host tests prove physical I2C |
| Ranging validation | valid range, impossible/negative value, stale sample, low confidence, timeout, outlier |
| Pairwise matrix | normalized node keys, update/stale/missing edge, partial matrix |
| CS scheduler | every available node gets initiator turn; reflector pairing; timeout/retry; join/leave; deterministic schedule independent of four-remotes assumption |
| Error handling | malformed transport payload, reset during session, partial recovery without clearing sticky status |

Native tests do not prove GPIO, the physical GY-BMI160, I2C electrical
behavior or radio ranging.

### C. Automatic `Tests: All (no flash)` boundary

The project-wide automatic test path must remain hardware-free.

Important PlatformIO rule: `pio test --without-uploading` prevents firmware
upload, but an embedded test can still require a physical test/serial port.
Therefore the automatic nRF `test` action must execute only test suites that
have been explicitly qualified as software/native tests. Physical target/HIL
suites must not become part of `Tests: All (no flash)` just because a new
`test_*` directory appears.

A future host/native-only PlatformIO environment is permitted if that is the
cleanest way to execute software tests. Such an environment is a test harness,
not another production firmware variant.

### D. One-board HIL

For a physical plain XIAO nRF54L15, distinguish main and remote hardware
profiles.

Common one-board checks:

- wrapper upload, boot and USB logging;
- identity/reset behavior;
- I2C target visibility/error handling;
- Bluetooth discovery/connection lifecycle.

Remote hardware checks additionally require:

- P1.04-P1.07 external LEDs;
- external GY-BMI160 presence on local D11/D12 I2C;
- valid accelerometer and gyroscope samples;
- static vertical, controlled tilt and return-to-vertical behavior;
- BMI160 missing/timeout/bad-data behavior;
- warning/error latch persistence.

Main-enclosure checks must prove:

- boot/communication with no external four-LED assembly;
- boot/communication with no external GY-BMI160;
- absence of those remote-only devices is not logged as a fatal main-node
  hardware error.

### E. Multi-node HIL

For two or more physical nRF boards, cover:

- basic two-node Channel Sounding pair;
- repeated pairwise ranging and quality/confidence;
- initiator/reflector rotation so every connected node initiates;
- distance-matrix updates;
- disconnect/reconnect;
- reboot of one node while others remain available;
- missing-node startup and recovery;
- warning/error indications on separate remote nodes.

### F. Full ESP32 + TCA topology HIL

Cover:

- ESP32 upstream I2C ownership;
- main nRF always visible;
- TCA9548A channel selection/deselection;
- one remote nRF per selected channel;
- remote BME280 on independent 3.3 V power;
- local remote GY-BMI160 on that node's D11/D12 bus, not the ESP32/TCA bus;
- approved solution for possible identical nRF target addresses;
- ESP32/nRF contracts carrying identity, topology, orientation and ranging
  results without breaking seismic acquisition.

## 7. Multi-node matrix

The implementation/test harness must use discovered node count rather than a
hard-coded four-remote assumption:

| Topology | Physical nRF boards | Minimum purpose |
| --- | ---: | --- |
| main only | 1 | boot, no-remote topology, no remote-only IMU/LED requirement |
| 1 main + 1 remote | 2 | discovery, one GY-BMI160, first ranging pair and role rotation |
| 1 main + 2 remotes | 3 | multiple pairs, partial matrix, reconnect |
| 1 main + 3 remotes | 4 | channel mapping and multi-node scheduling |
| 1 main + 4 remotes | 5 | diploma reference topology and complete ranging cycle |
| supported N beyond four | 1 + N | capacity limits and graceful handling when hardware exists |

Five physical boards must never be required by ordinary software-only CI.

## 8. Regression contract

Future nRF changes must gate:

- ESP32-S3 configure/build and existing host/component tests;
- nRF wrapper production build;
- qualified nRF software/native tests;
- WebUI tests/build/typecheck as currently defined;
- protocol/schema contracts between ESP32 and nRF when introduced;
- architecture/pin/config consistency;
- one-production-firmware/no-central-remote-variant invariant.

Physical nRF HIL is a separate explicit tier. Missing hardware is `BLOCKED`,
not an implicit pass.

## 9. Evidence vocabulary

Keep evidence types separate:

- `COMPILES`: wrapper build/link completed;
- `HOST TESTED` / `NATIVE TESTED`: deterministic software tests passed;
- `TARGET TESTED`: image ran on one physical target with machine-observable
  evidence;
- `HIL TESTED`: a named physical scenario passed;
- `PHYSICALLY VERIFIED`: a named physical observation was confirmed.

A successful build or native simulation never proves GY-BMI160 wiring, GPIO
LED behavior, physical Channel Sounding or real TCA topology.

## 10. Open questions / implementation gates

1. **I2C identity/addressing:** choose and validate how simultaneously visible
   nRF targets avoid/address collisions before I2C/TWIS implementation.
2. **GY-BMI160 module details:** verify the exact module's I2C address strap,
   onboard pull-ups, power behavior and whether interrupt lines are necessary.
3. **BMI160 Zephyr integration:** verify `bosch,bmi160` driver/Kconfig and the
   XIAO second-I2C controller/device-tree setup in the exact installed Seeed
   platform/framework version.
4. **Channel Sounding maturity:** qualify initiator/reflector support,
   measurement format and scheduling limits on the selected package.
5. **Geometry contract:** define distance-matrix schema, confidence and
   missing/outlier policy before feeding localization/WebXR.
6. **Tilt UX:** approve filter, calibration, threshold/warning band,
   hysteresis/debounce, blink mapping, saturation and IMU-failure output.
7. **Combined logger LEDs:** confirm red-only priority versus simultaneous
   yellow+red.
8. **Automatic test selection:** qualify a truly hardware-free native/host
   execution route before adding executable nRF software suites to `Tests: All`.

## 11. Official references checked

- [Seeed XIAO nRF54L15 getting started and pin map](https://wiki.seeedstudio.com/xiao_nrf54l15_sense_getting_started/)
- [Seeed XIAO nRF54L15 PlatformIO configuration](https://wiki.seeedstudio.com/xiao_nrf54l15_with_platform_io/)
- [Seeed platform-seeedboards](https://github.com/Seeed-Studio/platform-seeedboards)
- [Bosch BMI160 datasheet](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmi160-ds000.pdf)
- [Zephyr `bosch,bmi160` binding](https://docs.zephyrproject.org/latest/build/dts/api/compatibles/bosch%2Cbmi160.html)
- [Zephyr Channel Sounding sample](https://docs.zephyrproject.org/latest/samples/bluetooth/channel_sounding/README.html)
- [Nordic Bluetooth Channel Sounding overview](https://www.nordicsemi.com/Products/Wireless/Bluetooth-Low-Energy/Channel-Sounding)
- [PlatformIO `pio test`](https://docs.platformio.org/en/latest/core/userguide/cmd_test.html)
