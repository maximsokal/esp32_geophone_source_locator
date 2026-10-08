# nRF54L15 subproject

This is the independent XIAO nRF54L15 firmware subtree used by
`esp32_geophone_source_locator`. The repository root remains the ESP-IDF
ESP32-S3 project. **nRF Connect SDK v3.4.0 is the canonical production build
and flash toolchain for the XIAO firmware.** The historical PlatformIO + Zephyr
4.2.1 path is retained only as an explicitly named compatibility smoke build;
it is not an authoritative production artifact.

## Architecture rule

There is one production source tree, one production NCS v3.4.0 configuration
shape and one common runtime firmware image for every XIAO nRF54L15. Runtime
identity, MAIN/REMOTE wired topology and Channel Sounding initiator/reflector
role are not compile-time variants. Do not create separate `central`, `remote`,
`initiator` or `reflector` production environments or firmware copies.

The canonical application sources remain under `nRF54L15/src`, `include` and
`zephyr`. The pinned NCS wrapper builds the upstream board target
`xiao_nrf54l15/nrf54l15/cpuapp`. Generated NCS output belongs under
`%USERPROFILE%\.ncs-workspaces\esp32_geophone_source_locator`, outside the
repository.

The wired topology has one main-enclosure node on the ESP32-S3 upstream I2C
bus and one remote node per connected geophone behind the TCA9548A. With all
downstream channels disabled, the main node remains visible; selecting one mux
channel exposes that REMOTE while MAIN stays visible. ESP32-S3 remains the
wired-I2C controller. Every nRF reset starts UNASSIGNED at discovery address
`0x30`; a validated ASSIGN moves the main node to `0x31` or a REMOTE to
`0x32`. The implemented ESP32 `nrf_topology_service` performs MAIN-first
reconciliation, one-hot TCA scanning/assignment, warm-reset acceptance and
conflict detection. The byte-level bootstrap contract is in
[`../docs/nrf-upstream-bootstrap-protocol.md`](../docs/nrf-upstream-bootstrap-protocol.md),
and the ESP32 topology behavior is in
[`../docs/nrf-topology-service.md`](../docs/nrf-topology-service.md).
On each REMOTE, the private second I2C bus is controlled by that nRF54L15 and
carries the local GY-BMI160 and BME280. Both are powered from the XIAO
`3V3_OUT` rail; RJ45 pin 2 (+3.3 V) is `RESERVED/NC`, and must not be tied to
`3V3_OUT`.

## Actual board and external remote sensors

The project hardware uses the **plain Seeed Studio XIAO nRF54L15, not the
XIAO nRF54L15 Sense**. The plain board has no built-in IMU. Do not use the
Sense-only `LSM6DS3TR-C`/`imu0` device as the source of remote geophone
verticality.

Each remote geophone node instead uses an external **GY-BMI160** 6-axis IMU
and BME280 connected locally to that remote XIAO. The preferred wiring is the
XIAO's second I2C controller so the local sensors do not consume the SPI pins
and do not share the ESP32/TCA upstream I2C pair:

| XIAO pin | nRF54L15 pin | Remote use |
| --- | --- | --- |
| `D4 / SDA-0` | `P1.10` | shared upstream/TCA I2C SDA to ESP32; `i2c22` TWIS target |
| `D5 / SCL-0` | `P1.11` | shared upstream/TCA I2C SCL to ESP32; `i2c22` TWIS target |
| `D11 / SCL-1` | `P0.03` | local GY-BMI160 + BME280 I2C SCL; `i2c30` TWIM controller |
| `D12 / SDA-1` | `P0.04` | local GY-BMI160 + BME280 I2C SDA; `i2c30` TWIM controller |

The selected GY-BMI160 breakout is planned to use its `SCL`/`SDA` pins on
D11/D12, `3V3` from XIAO `3V3_OUT`, `VIN` NC, `CS/CSB` at 3.3 V for I2C and
`SA0/SDO` at GND for address `0x68`. `INT1`, `INT2` and auxiliary
`SCX`/`SDX`/`OCS` connections are NC for the initial assembly. Verify these
module-specific labels, pull-ups and straps against the physical breakout
before assembly.

The BME280 shares this local bus and is part of the remote nRF sensor snapshot;
it is not an upstream TCA device. Runtime probing accepts either Bosch I2C
address (`0x76` first, then `0x77`) so the firmware does not depend on the
breakout SDO strap. Each requested environment snapshot uses forced mode with
temperature x2, pressure x16, humidity x1 and IIR filter x16. The calculated
measurement time is 46.1 ms, so the driver waits 47 ms and then uses a bounded
status poll rather than an unbounded wait. Exact breakout pull-ups still need
physical verification. Any history required by the project belongs in nRF RAM,
not in an assumption that the BME280 stores a project history.

The external sensors are powered from the XIAO board `3V3_OUT` output and GND,
not from a firmware GPIO. Validate total load and transient margin in HIL.
The main-enclosure nRF54L15 has no geophone whose verticality must be measured
and therefore does not require either remote sensor assembly.

Because MAIN and REMOTE use the same production firmware while their external
sensor hardware differs, GY-BMI160 and BME280 are treated as **optional runtime
peripherals**, not as always-present Zephyr DeviceTree sensor devices. The
private `i2c30` controller is available in every image. A successful REMOTE
assignment activates a dedicated low-priority sensor worker using a copied
assignment snapshot; MAIN and UNASSIGNED nodes do not touch the private sensor
bus. IMU and environment sources are prepared independently, an initial sample
is produced when at least one source is available, and a failed source can be
retried on a later explicit refresh. Missing or faulty sensor hardware is
therefore represented as partial/no-data sensor state rather than a MAIN boot
failure.

The upstream target and local sensor controller use distinct peripheral
instances. The project overlay switches `i2c22` on D4/D5 (`P1.10/P1.11`) to
`nordic,nrf-twis`, while `i2c30` remains the private TWIM/controller for
D11/D12. The `nrf_upstream_target_zephyr` adapter uses Zephyr's standard
`i2c_target_register()` / `i2c_target_unregister()` target API in buffer mode
and exposes lifecycle, re-addressing with rollback, and raw hardware-ID access.
The adapter itself remains address/protocol-neutral; `nrf_upstream_protocol`
defines bootstrap addresses/frames plus SENSOR_REFRESH and the REMOTE sensor
extension, while `nrf_bootstrap_runtime_zephyr` owns the bounded
callback-to-worker handoff and deferred re-addressing. The TWIS buffer is
explicitly 128 bytes.

REMOTE reads prepare a fixed 120-byte v2 frame whose first 28 bytes are exactly
the legacy STATUS frame, so existing topology reads remain backward-compatible;
the full extension carries request/sequence/validity/fault state, nRF
completion uptime, BMI160/BME280 values, absolute altitude, and a versioned
height-reference-relative Z value with its own full-frame CRC. The decoder
continues to accept v1 112-byte frames. The
ESP32 `nrf_topology_service` consumes this frame through a bounded
select/identity-check/refresh/poll/close transaction and exposes only cached
results/non-blocking refresh requests to callers. See
[`../docs/nrf-topology-sensor-consumer.md`](../docs/nrf-topology-sensor-consumer.md).
The nRF uptime and ESP32 receipt uptime are separate local clock domains and
are not geophone TDOA timestamps. Physical 28-byte early-STOP followed by a
clean next TWIS transaction still requires HIL validation.

For a stationary geophone, the accelerometer gravity vector is the primary
verticality reference. The current REMOTE sensor worker includes a pure integer
orientation estimator and uses gyro only as a motion/stability gate. Periodic
orientation work is serialized inside that same worker and performs an
IMU-only BMI160 read; it never starts a BME280 forced conversion. Aggregate
SENSOR_REFRESH samples are reused by the estimator instead of immediately
issuing a redundant BMI160 read. The signed reference axis defaults to
`UNCONFIGURED` until the installed BMI160-to-geophone axis is physically
recorded/HIL-verified, so the firmware does not guess X/Y/Z orientation.

See the authoritative topology, Channel Sounding, external GY-BMI160 and LED
contract in [`../ARCHITECTURE.md`](../ARCHITECTURE.md). The detailed test and
HIL matrix is in
[`../docs/nrf54l15-test-task.md`](../docs/nrf54l15-test-task.md).

## Channel Sounding toolchain boundary

Channel Sounding support is proven only on **NCS v3.4.0 / Zephyr 4.4.0 with
Nordic SoftDevice Controller**. `nRF54L15/ncs/channel_sounding.conf` remains the
historical MAX-45 additive proof fixture until the real MAX-51 Bluetooth/RAS
adapter promotes reviewed Channel Sounding options into the permanent
production configuration.

Therefore, as of MAX-96:

- `ncs.ps1 build` = canonical production image from `zephyr/prj.conf` only;
- `ncs.ps1 cs-proof` = MAX-45 SoftDevice/Channel-Sounding proof image;
- `ncs.ps1 compare` = build both and report their ELF sizes;
- `ncs.ps1 probe` = inspect the existing MAX-45 proof artifact;
- the proof overlay is **not** silently included in the production build.

The platform-neutral `nrf_cs_scheduler` is already compiled in the common
application. Real RAS/CS execution is the separate MAX-51 slice; multi-node
execution authority/handoff is tracked separately so `BT_MAX_CONN=1` does not
turn into an implicit distributed BLE race.

## Remote external LEDs

Remote external LED wiring is:

| Pin | LED |
| --- | --- |
| `P1.04 / D0` | BLUE |
| `P1.05 / D1` | GREEN |
| `P1.06 / D2` | YELLOW |
| `P1.07 / D3` | RED |

The main-enclosure board has none of these four external LEDs. The production
firmware keeps onboard `led0` as a board/toolchain heartbeat, drives the REMOTE
blue LED on D0 from the sensor worker, and applies logger status to D1-D3. The
logger GPIO backend tolerates missing or unavailable pins so MAIN and LED-less
boards can continue booting and communicating.

**ASSUMED active-high, same as BLUE; NOT verified (MAX-43); verify with a current-limited test before trusting.** This polarity is configured only in
DeviceTree. The C GPIO glue reads each pin's DeviceTree flags.

BLUE is steady for `ACCEPTED`, blinks at 2 Hz for `STABILIZING` or `TILTED`, and
is off for `FAULT`.

REMOTE logger LED state is:

| Condition | GREEN D1 | YELLOW D2 | RED D3 |
| --- | --- | --- | --- |
| READY, no latched fault (sensor preparation may be pending) | On | Off | Off |
| WARNING present for less than 2000 ms | On | Off | Off |
| WARNING confirmed for 2000 ms, without higher severity | Off | On | Off |
| ERROR or FATAL, including watchdog stale | Off | Off | On |
| ERROR/FATAL while WARNING is latched | Off | Off | On |
| MAIN, UNASSIGNED, or not READY | Off | Off | Off |

Warnings need 2000 ms of continuous confirmation. Errors and fatals apply
immediately. RED is shown alone and suppresses YELLOW. Logger latches persist
until reset. GREEN means “no latched fault,” not that sensor preparation has
completed. An unattempted sensor is pending, not a fault. A failed BMI160 init
or failed required BME280 init produces RED immediately; runtime IMU/ENV fault
bits count only after their sensor is READY. `LOG_RING_LOSS` latches WARNING
when `overwritten_total > 0` (internal logger losses also map to WARNING). The
128-record ring can overwrite during a bench run without an ESP32 collector,
producing a latched YELLOW; this is expected bench behavior.

The policy, Zephyr GPIO backend, and D1-D3 DeviceTree mapping are
**IMPLEMENTED-BUT-UNVERIFIED**. LED polarity and physical operation remain
unverified pending MAX-43 HIL. HIL checklist — **NOT RUN**: use a
current-limited test to confirm D1-D3 polarity and series-resistor wiring;
exercise green, warning confirmation, red priority, and reset behavior; confirm
MAIN and LED-less boards still boot and communicate.

## Boot lamp test and console status

At boot, the image briefly drives BLUE, GREEN, YELLOW, then RED for 300 ms
each, and turns each available pin off before normal runtime startup. The
test uses the same DeviceTree GPIO specs and polarity handling as the runtime
drivers. It does not verify LED polarity. If a lamp stayed dark, check wiring
and polarity (MAX-43).

Open the serial monitor from the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File nRF54L15/ncs.ps1 monitor -Port COMx -Baud 115200
```

`APP_LED_BOOT_LAMP_TEST` and `APP_NRF_VERBOSE_LOG` default to `y`;
`APP_NRF_STATUS_PRINT_PERIOD_MS` defaults to `5000`. Set the period to `0` to
disable periodic status, or set `APP_NRF_VERBOSE_LOG=n` to disable runtime
transactions after the first callback, change lines, and periodic status. The
first TWIS callback always prints one compact transaction line, including its
direction, length, up to eight bytes, and result. With verbose logging enabled,
the first 64 callback transactions get one compact line each. Later traffic
gets at most one summary per second; up to four error records per second print
immediately, with excess errors counted in the next summary. The fixed event
ring holds 64 records and reports any overflow in status.

For example, startup reports the backend result and each logger LED pin state,
then emits one `lamp_test` line per available LED. Main-loop change lines carry
uptime; a status line carries the latest state and counters:

```text
status: up=5000ms lifecycle=ready role=unassigned twis={addr_match=n/a,write_requested=n/a,write_received=0,write_bytes=0,read_requested=0,stop=n/a,callback_err=0,last_type=n/a,last_count=0,ring_dropped=0} sensors={imu=0/init=0,env=0/init=0} led={g=0,y=0,r=0,sev=none/source=none,blue=off} logs={overwritten=0,lost=0} led_counters={gpio_fail=0,policy_invalid=0,policy_time=0,blue_fail=0}
```

The TWIS buffer API exposes completed write buffers and read requests, so the
line counts those callbacks and received bytes. It does not expose address
match, write-requested, or stop callbacks; those fields are printed as `n/a`.
`callback_err` counts nonzero write-handling and read-response results. The
unconditional `twis: tx=1 ...` line identifies the first target callback and
includes its compact record even when verbose logging is disabled.

## Supported Windows production interface

From an ordinary PowerShell at the repository root:

```powershell
.\nRF54L15\ncs.ps1 build
.\nRF54L15\ncs.ps1 clean
.\nRF54L15\ncs.ps1 flash
.\nRF54L15\ncs.ps1 monitor -Port COM7
```

`ncs.ps1` is pinned to NCS v3.4.0. If `west` is not already available, the
wrapper relaunches itself through:

```powershell
nrfutil sdk-manager toolchain launch --ncs-version v3.4.0 -- ...
```

The checkout may contain spaces. On Windows the wrapper creates a temporary
no-space `SUBST` source alias; generated NCS output stays outside the repo in
`%USERPROFILE%\.ncs-workspaces\esp32_geophone_source_locator`.

`flash` is an explicit hardware action and is never part of `Tests: All (no
flash)`. `monitor` only opens the requested serial port and is not itself HIL
acceptance.

VS Code canonical tasks are:

- `Build: nRF54L15 production (NCS)`;
- `Tests: nRF54L15`;
- `nRF54L15: Flash firmware`;
- `nRF54L15: Monitor logs`.

## PlatformIO compatibility path

`platformio.ini` and `pio.ps1` are retained because they are still useful for
compatibility smoke checks against the older Seeed/Zephyr 4.2.1 package. They
are **not** the production build/flash authority after MAX-96.

The supported compatibility command is deliberately named as such in VS Code:

```text
nRF54L15: PlatformIO compatibility build
```

PlatformIO selects `zephyr/prj.platformio.conf`, a minimal shared-source
configuration for its Zephyr 4.2.1 package. The NCS-only SoftDevice/RAS/Channel
Sounding options remain exclusively in `zephyr/prj.conf`; this keeps the
compatibility smoke build useful without treating it as a production image.

Do not use `pio.ps1 upload` for the official firmware and do not interpret a
successful PlatformIO build as Channel Sounding controller evidence. The
canonical production artifact always comes from the pinned NCS v3.4.0 path.
