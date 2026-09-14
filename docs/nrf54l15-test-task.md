# Task: develop and run nRF54L15 tests

Status: **planning only**. This task prepares the future nRF54L15 firmware and
its verification work. It does not implement firmware, BLE, Bluetooth Channel
Sounding, I2C/TWIS, IMU, GPIO or LED drivers.

The task is deliberately limited to the independent `nRF54L15/` PlatformIO +
Zephyr project. The ESP-IDF firmware, WebUI implementation, hardware pin
assignments and existing ESP32 tests are not to be changed as part of this
planning task.

Related sources of truth:

- repository boundaries and target topology: [`ARCHITECTURE.md`](../ARCHITECTURE.md);
- current project-wide verification entry point: [`local-verification.md`](local-verification.md);
- evidence vocabulary and physical-proof boundary: [`verification-evidence-matrix.md`](verification-evidence-matrix.md);
- existing ESP32 firmware test layers: [`firmware-test-plan.md`](firmware-test-plan.md);
- nRF54L15 project entry point: [`../nRF54L15/README.md`](../nRF54L15/README.md).

## 1. Goal and non-goals

The future implementation/test work must make every available XIAO nRF54L15
usable as the same runtime-configured node in the geophone system. It must
provide the ranging and orientation evidence needed by the ESP32-S3 and the
later localization/3D-WebXR layers, while keeping the nRF project isolated
from the ESP-IDF root project.

This task does not authorize:

- production firmware implementation;
- separate `central` and `remote` source trees or PlatformIO environments;
- BLE or Channel Sounding implementation;
- I2C/TWIS protocol implementation or an invented address-allocation scheme;
- IMU calculations, GPIO drivers or LED drivers;
- ESP32-S3, WebUI or existing integration-code changes;
- flashing, erasing or other physical-board actions during this documentation task.

## 2. Current verified baseline

The repository currently contains only the nRF bring-up project:

```text
nRF54L15/
  platformio.ini
  pio.ps1
  src/main.c
  test/                 # present but currently has no test_* suites
  zephyr/CMakeLists.txt
  zephyr/prj.conf
```

`platformio.ini` currently has exactly one environment:

```ini
[env:seeed-xiao-nrf54l15]
platform = https://github.com/Seeed-Studio/platform-seeedboards.git
framework = zephyr
board = seeed-xiao-nrf54l15
```

The current `src/main.c` is a temporary board/toolchain blink using the board
`led0` alias. It is not the future external remote LED implementation and it
does not prove any nRF application feature.

### Required Windows entry point

The repository path contains spaces. The current Seeed PlatformIO + Zephyr
generation path fails when the source/build path is passed through the normal
Windows path in this checkout. `nRF54L15/pio.ps1` is therefore the supported
entry point:

```powershell
.\nRF54L15\pio.ps1 build
.\nRF54L15\pio.ps1 clean
.\nRF54L15\pio.ps1 upload
.\nRF54L15\pio.ps1 monitor
.\nRF54L15\pio.ps1 test
```

The wrapper maps the source to a temporary drive without spaces and places
PlatformIO workspace/build/libdeps under:

```text
%USERPROFILE%\.platformio-workspaces\nrf54l15
```

The test runner uses the new `test` wrapper action. It must not make a direct
`pio run -d ".\nRF54L15"` command the normal workflow. The action keeps the
SUBST/workspace workaround and invokes PlatformIO with
`--without-uploading`. The current repository has no `test/test_*` suites, so
the wrapper and project-wide runner report an explicit `SKIPPED` result until
the planned nRF test implementation adds them.

The baseline build was re-run on 2026-09-14 with the supported wrapper and
completed successfully. The observed local toolchain was PlatformIO Core
6.1.19, SeeedStudio platform 1.0.0 at
`1ec1287f8e4bc4067a6fd593991e36875aef989f`, and
`framework-zephyr-nrf54l15` package `3.40201.251021`, reporting Zephyr
4.2.1. These are verification snapshots for this checkout, not a promise
that an unpinned remote platform URL will never change.

The dedicated VS Code task `Tests: nRF54L15` launches
`nRF54L15/pio.ps1 -Action test`. The project-wide `Tests: All (no flash)` task
invokes the same nRF test boundary through `tools/run_suite.ps1`, after the
nRF wrapper build. Both entry points preserve the Windows path workaround and
do not upload a test image; an empty current test surface is reported as
`SKIPPED`.

The separate VS Code task `nRF54L15: Flash firmware` invokes
`nRF54L15/pio.ps1 -Action upload` for an explicitly connected physical XIAO
board. It is intentionally outside `Tests: All (no flash)` and is not an
automatic HIL test result by itself.

## 3. Architecture contract that tests must protect

### One firmware for every node

All XIAO nRF54L15 boards use:

- one source tree;
- one `platformio.ini` environment;
- one implementation;
- one firmware binary/configuration shape.

There must be no compile-time `central`/`remote` variants and no separate
PlatformIO environments for those roles. Runtime identity, topology and
current ranging role are data/state, not build selection.

### Physical topology

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
|           +-- selected channel -> remote nRF54L15 + remote sensors
|           +-- another channel  -> remote nRF54L15 + remote sensors
|           +-- ...
|
+-- existing diploma hardware
```

There is one main-enclosure nRF54L15 and one remote nRF54L15 per connected
remote geophone. Four remote geophones therefore mean five nRF54L15 boards in
the complete system.

The intended discovery concept is to disable all TCA9548A downstream
channels, identify the nRF target that remains visible on the upstream bus as
the main-enclosure node, and then select channels one at a time to identify
remote nodes. The main target remains visible regardless of mux selection;
remote targets appear only through their selected channel.

This is a topology concept, not an implemented protocol. Before any I2C/TWIS
implementation, the task must resolve the possibility that multiple nRF
targets have the same I2C address. The address, identity provisioning and
collision-avoidance mechanism are explicit architecture gates; no address or
resolution method may be invented in a test fixture.

ESP32-S3 remains the main controller and the wired-I2C owner. A remote BME280
has its own 3.3 V supply and is not powered by the nRF54L15. The nRF54L15 is
not to be assumed to be the BME280 I2C master. The exact BME280 address and
the final ESP32-side sensor contract remain separate integration questions.

### Bluetooth Channel Sounding purpose and role rotation

The target wireless technology is Bluetooth 6 Channel Sounding, not RSSI-only
positioning when Channel Sounding is available. The nRF nodes must eventually
provide pairwise range observations that can help the system reconstruct the
relative geometry of remote geophones and bind it to localization and 3D/WebXR
data.

The main-enclosure board is not a permanently privileged Bluetooth master.
During a complete measurement cycle, every connected nRF54L15 must become the
Channel Sounding initiator in a scheduled turn, with the other node serving as
reflector when appropriate. The future design/test contract must cover:

- Channel Sounding session scheduling;
- initiator/reflector role switching at runtime;
- pairwise ranging;
- measurement quality/confidence;
- aggregation of repeated measurements;
- missing, timed-out and outlier observations;
- a distance matrix for currently available nodes;
- node counts from zero remote nodes through the supported maximum;
- reconnect, reboot and partial-topology behavior.

The exact ranging algorithm, geometry solver and acceptance thresholds are
not specified here. The official Nordic material describes the initiator as
the side that obtains the raw Channel Sounding data and applies the
distance-estimation algorithm, while the reflector responds to the sounding
procedure. The official Zephyr sample also separates initiator and reflector
applications. This confirms the role model, not a project geometry algorithm.
Bluetooth Channel Sounding support in the controller, host and selected
Zephyr/Seeed package must be proven before an implementation claims that
generic Zephyr APIs are sufficient.

### Sense IMU and verticality

Seeed's official board specification distinguishes the boards: the plain XIAO
nRF54L15 has no built-in sensor, while **XIAO nRF54L15 Sense** has a 6-axis
`LSM6DS3TR-C` IMU. Therefore every remote board that is required to report
verticality must be physically confirmed to be the Sense variant. The
PlatformIO board ID used by this repository is `seeed-xiao-nrf54l15`; the
currently installed Seeed board files expose an `imu0` device-tree alias for
the Sense-oriented board definition. This mapping must be rechecked whenever
the Seeed platform package changes.

The official Seeed Zephyr example uses the generic sensor API:

- `DEVICE_DT_GET(DT_ALIAS(imu0))`;
- `sensor_sample_fetch_chan()`;
- `sensor_channel_get()` with `SENSOR_CHAN_ACCEL_XYZ` and
  `SENSOR_CHAN_GYRO_XYZ`;
- optional `sensor_trigger_set()` and `sensor_attr_set()` for trigger and
  sampling-frequency configuration.

In the currently installed Seeed board files, the `LSM6DS3TR-C` node is
exposed through Zephyr's `st,lsm6dsl` binding and `CONFIG_LSM6DSL` driver name,
with the Seeed example enabling `CONFIG_SENSOR`, `CONFIG_I2C`, `CONFIG_MFD`,
`CONFIG_REGULATOR` and the LSM6DSL trigger option. This is the Zephyr
compatibility/binding name used by that package; it does not change the
physical sensor identification to LSM6DSL. Revalidate this mapping after a
platform/framework update.

For static verticality, the accelerometer/gravity vector is the primary
measurement: at rest it gives the direction of gravity and therefore tilt.
The gyroscope measures angular velocity and is useful for detecting movement
or for a future sensor-fusion design; it does not by itself provide a stable
absolute static angle without integration drift. The future implementation
must not call a gyro mandatory for a stationary tilt decision without a
measured reason.

The project's mechanical reference is approximately 10 degrees maximum tilt.
It is not yet an approved single firmware threshold. The implementation task
must define and test the tilt threshold, warning zone, hysteresis/debounce,
filter type/window, invalid-sensor behavior and calibration assumptions as
explicit parameters.

### Remote LED hardware and logger semantics

Each remote node has four external LEDs:

| nRF54L15 pin | LED | Planned meaning |
| --- | --- | --- |
| `P1.04` | BLUE | verticality/tilt indication |
| `P1.05` | GREEN | normal logger state |
| `P1.06` | YELLOW | latched warning |
| `P1.07` | RED | latched error |

The main-enclosure nRF54L15 has none of these four external LEDs. The common
firmware must therefore treat LED hardware as optional: a main node must boot
and communicate when those GPIOs are physically absent, while a remote node
uses them when present. The current bring-up's board `led0` alias is the
onboard user LED and must not be confused with this external four-LED remote
assembly.

BLUE is an independent orientation indicator. The intended behavior is steady
BLUE when the filtered tilt is within the accepted vertical state, and a
blink whose rate increases with the measured error outside that state. The
future task must define, measure and test the angle-to-frequency mapping,
saturation, filtering, hysteresis/debounce and IMU-failure indication. No
frequency or transition angle is approved by this planning document.

The ESP32 `status_indicator` implementation was inspected before writing this
task. It consumes explicit application status decisions rather than blindly
turning every `ESP_LOG` line into an LED event. WARNING and ERROR are sticky
until power cycle; a later NORMAL/CLEAR_NON_STICKY event does not clear either
latch. Its effective visible state has priority `ERROR > WARNING > NORMAL`.
The current ESP32 board has no connected status LED GPIOs, so this is a state
and semantics contract, not a physical ESP32 LED proof.

The future remote state contract must therefore test:

1. boot/normal -> GREEN steady;
2. warning -> YELLOW latched until reset/power cycle;
3. error -> RED latched until reset/power cycle;
4. a later successful event does not clear a warning/error latch;
5. error after warning follows the explicitly approved effective-state rule;
6. warning and error together have an explicitly documented physical UX.

The last item remains **TBD for confirmation**. The current ESP state machine
selects ERROR as the effective state, but the remote board has independent
YELLOW and RED outputs. The implementation task must decide and document
whether the physical result is red-only priority or simultaneous yellow+red,
then add the matching regression. It must not silently infer this from the
LED names.

## 4. Test levels and future work packages

### A. Build and configuration tests

The build/configuration package must prove:

1. `nRF54L15/` is an independent PlatformIO project and the repository root
   remains an ESP-IDF project, not a PlatformIO project.
2. `platformio.ini` has one nRF environment and no central/remote environments.
3. The supported build command is `.\nRF54L15\pio.ps1 build`.
4. `.\nRF54L15\pio.ps1 clean` followed by `build` succeeds from the
   repository path containing spaces.
5. A second unchanged `build` is an incremental build and still succeeds.
6. The nRF build is independent of the ESP-IDF configure/build and of the
   WebUI install/build.
7. Generated build, workspace, dependency and Zephyr files stay outside the
   Git working tree, except for intentionally tracked source/configuration.
8. The build report records board ID, Seeed platform revision, framework
   package/Zephyr version and toolchain versions so package drift is visible.

The future test must use explicit read-only checks such as `git status
--short`, `git ls-files` and an allowlisted generated-output check. It must
not clean or discard unrelated repository work as part of verification.

### B. Unit tests

The future nRF unit suite must be host-runnable and deterministic wherever the
logic does not require hardware. At minimum, cover:

| Area | Required cases |
| --- | --- |
| Node identity/state machine | boot, unassigned, main, remote, unavailable, reconnect and reset transitions; invalid transitions rejected |
| Runtime role/topology | main remains visible with all mux channels closed; one selected remote appears; disappearing/reappearing channel; zero through maximum supported remotes; duplicate/ambiguous identity is an error, not an invented choice |
| Logger LED latches | boot GREEN; WARNING latch; ERROR latch; normal/success does not clear; error priority; simultaneous warning/error according to the approved UX |
| Tilt and BLUE mapping | steady vertical state; below/at/above threshold; warning zone; monotonic blink mapping; maximum-rate saturation; invalid IMU input; no mode chatter at a boundary |
| Filtering/hysteresis | startup fill, noisy samples, stationary gravity, motion entry/exit, hysteresis equality sides, reset and sensor timeout |
| Ranging validation | valid sample, bad timestamp, impossible/negative distance, low confidence, timeout, missing reflector, malformed packet and outlier rejection |
| Pairwise data | node-key normalization, symmetric lookup policy, duplicate update, stale sample, missing edge and distance-matrix construction |
| Channel Sounding scheduler | every available node gets an initiator turn; reflector pairing; no duplicate active session; timeout/retry; node joins/leaves; deterministic order without assuming four remotes |
| Error handling | malformed I2C/BLE payload, transport failure, reset during session, partial matrix and recovery without clearing sticky logger state |

Tests must exercise equality and strict-inequality boundaries for every chosen
threshold. Values not approved by project documentation remain parameters or
TBD fixtures, not hidden constants in the test expectations.

### C. Hardware-independent/native tests

The future implementation task must first prove which native route works with
the installed Seeed PlatformIO + Zephyr 4.2.1 environment:

- Zephyr `ztest` for Zephyr-integrated state and API contracts;
- Zephyr `native_sim` when a full Zephyr host context is useful; it runs a
  Zephyr application as a host executable but does not emulate this board's
  physical GPIO, IMU, I2C or radio;
- Zephyr `unit_testing` only where its host/platform restrictions are
  acceptable. Current Zephyr documentation describes this target as supported
  on Linux, so Windows execution must not be assumed without a container/VM
  proof;
- PlatformIO `pio test` / Local Test Runner through `nRF54L15/pio.ps1 test`,
  with the no-space path wrapper preserved and upload disabled. Whether a
  future suite can execute natively in this exact Seeed/Zephyr environment or
  needs a separate host runner remains an implementation-stage qualification.

The native package must separate pure policy/data tests from board-dependent
tests. A native pass is `HOST TESTED`/`NATIVE TESTED`, never `HIL TESTED` or
`PHYSICALLY VERIFIED`. The current wrapper action is only the stable launch
boundary; it does not claim that the empty current test surface is a native
pass and must not bypass the no-space path handling.

### D. Hardware-in-the-loop tests

HIL is a separate, explicitly authorized package. Each run must preserve the
firmware SHA, board variant, platform/framework versions, wiring, test
parameters, complete USB log and final result. A missing board, missing port or
missing telemetry is `BLOCKED`, not a pass.

For one real XIAO nRF54L15, cover:

- wrapper-based upload, boot and USB logging;
- no-flash boot observation where an already-installed image is used;
- configuration/identity report and reset behavior;
- remote external LED GPIO test for `P1.04`–`P1.07`;
- main-enclosure boot with the four external LED outputs absent;
- Sense IMU availability, sample validity and calibration/invalid-data path;
- static vertical orientation, controlled tilt and return-to-vertical behavior;
- I2C target visibility and error handling;
- Bluetooth discovery and connection lifecycle;
- basic two-node Channel Sounding pair when the controller/host feature is
  enabled and qualified;
- warning/error latching across successful operations and reset.

For two or more real nRF boards, cover:

- pairwise ranging and repeated-measurement quality;
- distance-matrix updates and missing/outlier handling;
- initiator/reflector rotation so every connected node initiates;
- disconnect/reconnect of a remote node;
- reboot of one node while others remain available;
- missing-node startup and runtime recovery;
- mixed warning/error indications on separate remote boards.

For the complete wired topology, additionally cover:

- ESP32 upstream I2C ownership with the main nRF always visible;
- TCA9548A channel selection and deselection;
- one remote nRF per selected channel;
- BME280 behavior on its independent 3.3 V supply;
- no accidental assumption that an nRF target powers or masters the BME280;
- the approved solution for identical possible nRF I2C addresses;
- ESP32/nRF contracts carrying node identity, topology, orientation and
  ranging results without changing existing ESP32 acquisition behavior.

The current ESP32 COM4/COM6 HIL runners do not prove any of these nRF
properties. nRF HIL evidence must use a dedicated future runner and must be
reported separately from ESP32 HIL.

### E. Multi-node test matrix

The implementation and test harness must be parameterized by discovered node
count, not hard-coded to four remotes:

| Topology | Physical nRF boards | Minimum purpose |
| --- | ---: | --- |
| main only (0 remote) | 1 | boot, no-remote topology, main-without-LED behavior, empty matrix |
| 1 main + 1 remote | 2 | first topology discovery, basic pair and role rotation |
| 1 main + 2 remotes | 3 | more than one pair, partial missing/reconnect and matrix updates |
| 1 main + 3 remotes | 4 | channel mapping and multi-node scheduling |
| 1 main + 4 remotes | 5 | diploma reference topology, complete rotation and geometry input |
| supported N beyond four | 1 + N | capacity/limits and graceful handling; execute when hardware exists |

No ordinary software-only CI job may require five physical boards. The first
five rows are evidence tiers; they are not a reason to block host/native build
or unit CI when hardware is unavailable.

### F. Regression and integration tests

For every future nRF implementation change, the regression gate must check:

- ESP32-S3 configure/build and existing host/component tests;
- nRF wrapper build and native/unit tests;
- WebUI tests/build/typecheck/lint as currently defined;
- protocol/schema/serialization contracts between ESP32 and nRF;
- architecture, hardware-pin and configuration-document consistency;
- one-firmware/no-central-remote-variant invariants.

This cross-project regression is a contract for the future master runner. It
does not authorize changing ESP32 or WebUI implementation in this task.

## 5. Evidence and completion criteria

Keep the project's existing evidence vocabulary separate:

- `COMPILES`: wrapper build and link completed;
- `HOST TESTED` / `NATIVE TESTED`: deterministic host or native tests passed;
- `TARGET TESTED`: image was run on one target with machine-observable logs;
- `HIL TESTED`: an explicitly defined physical scenario passed;
- `PHYSICALLY VERIFIED`: only a named physical observation, with its own
  evidence, was confirmed.

Replay, a native simulator, or a successful compile must never be promoted to
physical HIL evidence. The task is complete only when the future implementation
has:

1. one firmware/environment proven across the required runtime roles;
2. the build/configuration and unit/native packages passing;
3. the one-board, multi-board and full-topology HIL results recorded where
   hardware is available;
4. all missing hardware reported as explicit `BLOCKED` tiers;
5. no unresolved I2C-address collision hidden by a test fixture;
6. no undocumented tilt/blink/status threshold or simultaneous LED decision;
7. project-wide regression evidence for ESP32, nRF, WebUI and integration
   contracts.

## 6. Open questions and technical risks

These are gates for the future implementation task, not decisions made by
this document:

1. **Board variant:** physically confirm whether every target that needs an
   IMU is XIAO nRF54L15 Sense. The plain board is documented by Seeed as
   having no built-in sensor.
2. **Platform drift:** `platformio.ini` currently references the Seeed Git
   repository without a commit pin. Reconfirm board files, Zephyr version,
   Channel Sounding Kconfig/controller support and IMU alias after every
   package refresh.
3. **Windows path handling:** generated Zephyr files have already failed under
   paths with spaces. Keep `pio.ps1` the interface and verify clean and
   incremental behavior from the real checkout path.
4. **I2C identity/addressing:** identical target addresses may make the
   discovery concept ambiguous. Choose and validate a hardware/protocol
   resolution before writing I2C/TWIS code.
5. **BME280 ownership/power:** confirm the final downstream wiring and ESP32
   protocol without assigning BME280 power or master ownership to nRF.
6. **Channel Sounding maturity:** verify the exact controller/host support,
   initiator and reflector roles, measurement format, scheduling limits and
   qualification on the selected Zephyr/Seeed package. Do not substitute RSSI
   as an undocumented final design.
7. **Geometry contract:** define the distance-matrix schema, coordinate-frame
   convention, quality/confidence semantics and missing/outlier policy before
   connecting ranges to localization/WebXR.
8. **Tilt UX:** approve filter, threshold, warning zone, hysteresis, blink
   mapping, saturation and IMU-failure indication. Keep the approximately
   10-degree mechanical norm distinct from those firmware parameters.
9. **Combined logger LEDs:** confirm red-only priority versus simultaneous
   yellow+red when both sticky conditions exist.
10. **Electrical LED contract:** confirm LED active level, resistor/wiring
    behavior and whether unpopulated main-node pins are represented as absent
    optional hardware rather than required devices.
11. **Test runner qualification:** determine whether the future native suites
    execute through PlatformIO in the installed Seeed/Zephyr environment or
    need a ztest/native_sim host route. Keep the existing `pio.ps1 test`
    boundary and ensure the software-only master runner never requires a
    physical board merely because a target/HIL suite exists.

## 7. Official references checked on 2026-09-14

- [Seeed XIAO nRF54L15(Sense) getting started and pin map](https://wiki.seeedstudio.com/xiao_nrf54l15_sense_getting_started/)
- [Seeed XIAO nRF54L15 Sense built-in sensor example](https://wiki.seeedstudio.com/xiao_nrf54l15_sense_built_in_sensor/)
- [Seeed XIAO nRF54L15 PlatformIO configuration](https://wiki.seeedstudio.com/xiao_nrf54l15_with_platform_io/)
- [Seeed platform-seeedboards](https://github.com/Seeed-Studio/platform-seeedboards)
- [Zephyr XIAO nRF54L15 board documentation](https://docs.zephyrproject.org/latest/boards/seeed/xiao_nrf54l15/doc/index.html)
- [Zephyr sensor API fetch/get and triggers](https://docs.zephyrproject.org/latest/hardware/peripherals/sensor/fetch_and_get.html)
- [Zephyr Channel Sounding sample](https://docs.zephyrproject.org/latest/samples/bluetooth/channel_sounding/README.html)
- [Nordic Bluetooth Channel Sounding overview](https://www.nordicsemi.com/Products/Wireless/Bluetooth-Low-Energy/Channel-Sounding)
- [Zephyr ztest](https://docs.zephyrproject.org/latest/develop/test/ztest.html) and [native_sim](https://docs.zephyrproject.org/latest/boards/native/native_sim/doc/index.html)
- [PlatformIO `pio run` and project/environment options](https://docs.platformio.org/en/latest/core/userguide/cmd_run.html), [unit-test runner](https://docs.platformio.org/en/stable/advanced/unit-testing/runner.html) and [`pio test`](https://docs.platformio.org/en/latest/core/userguide/cmd_test.html)
