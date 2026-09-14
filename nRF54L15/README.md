# nRF54L15 subproject

This is an independent PlatformIO + Zephyr project for all Seeed Studio XIAO
nRF54L15 nodes used by `esp32_geophone_source_locator`. The repository root
remains the ESP-IDF project and is not a PlatformIO project.

## Architecture rule

There is one production source tree, one production PlatformIO environment and
one firmware binary for every XIAO nRF54L15. Runtime identity,
main/remote topology and initiator/reflector role are not compile-time
variants. Do not create separate `central` and `remote` environments or
firmware copies. A future host/native-only test environment is allowed if it is
required to execute software tests without hardware; it must never become a
second production firmware variant.

The planned topology has one main-enclosure node on the ESP32-S3 upstream I2C
bus and one remote node per connected geophone behind the TCA9548A. With all
downstream channels disabled, the main node remains visible; selecting a mux
channel exposes that remote node. ESP32-S3 remains the wired-I2C controller.
The possible identical-I2C-address problem must be resolved before an I2C/TWIS
protocol is implemented. A remote BME280 has independent 3.3 V power and is
not powered or assumed to be mastered by this nRF node.

## Actual board and external IMU

The project hardware uses the **plain Seeed Studio XIAO nRF54L15, not the
XIAO nRF54L15 Sense**. The plain board has no built-in IMU. Do not use the
Sense-only `LSM6DS3TR-C`/`imu0` device as the source of remote geophone
verticality.

Each remote geophone node instead uses an external **GY-BMI160** 6-axis IMU
connected locally to that remote XIAO. The preferred wiring is the XIAO's
second I2C controller so the local IMU does not consume the SPI pins and does
not share the ESP32/TCA upstream I2C pair:

| XIAO pin | nRF54L15 pin | Remote use |
| --- | --- | --- |
| `D4 / SDA-0` | `P1.10` | shared upstream/TCA I2C SDA to ESP32 |
| `D5 / SCL-0` | `P1.11` | shared upstream/TCA I2C SCL to ESP32 |
| `D11 / SCL-1` | `P0.03` | local GY-BMI160 I2C SCL |
| `D12 / SDA-1` | `P0.04` | local GY-BMI160 I2C SDA |

The GY-BMI160 is powered from the remote 3.3 V/GND rails. Its primary BMI160
interface supports I2C or SPI, but this project chooses local I2C on D11/D12
to reduce pin and wiring pressure inside the remote enclosure. The external
IMU is required only on remote geophone nodes; the main-enclosure nRF54L15 has
no geophone whose verticality must be measured and therefore does not require
the GY-BMI160.

For a stationary geophone, the accelerometer gravity vector is the primary
verticality reference. Gyroscope data is useful for motion detection and
optional sensor fusion but is not, by itself, an absolute static tilt
reference.

See the authoritative topology, Channel Sounding, external GY-BMI160 and LED
contract in [`../ARCHITECTURE.md`](../ARCHITECTURE.md). The dedicated future
work and test matrix are in
[`../docs/nrf54l15-test-task.md`](../docs/nrf54l15-test-task.md).

## Remote external LEDs

Remote external LED wiring is:

| Pin | LED |
| --- | --- |
| `P1.04 / D0` | BLUE |
| `P1.05 / D1` | GREEN |
| `P1.06 / D2` | YELLOW |
| `P1.07 / D3` | RED |

The main-enclosure board has none of these four external LEDs. The current
`src/main.c` is only a temporary board/toolchain blink through the onboard
`led0` alias; it is not the future logger/orientation implementation.

BLUE is the remote geophone orientation indicator: steady while the filtered
orientation is accepted as vertical and blinking progressively faster as tilt
error increases outside the accepted state. GREEN/YELLOW/RED implement the
remote logger-status semantics documented in `ARCHITECTURE.md`.

## Supported Windows build interface

The checkout path contains spaces, and direct Seeed Zephyr generation through
that path is unreliable. From the repository root, use the wrapper:

```powershell
.\nRF54L15\pio.ps1 build
.\nRF54L15\pio.ps1 clean
.\nRF54L15\pio.ps1 upload
.\nRF54L15\pio.ps1 monitor
.\nRF54L15\pio.ps1 test
```

The wrapper uses a temporary no-space `SUBST` drive and keeps generated
PlatformIO/Zephyr files under `%USERPROFILE%\.platformio-workspaces\nrf54l15`.
Do not use a direct `pio run -d ".\nRF54L15"` command as the normal workflow.
The `test` action is the supported automatic software-test entry point and
must remain hardware-free. Physical target/HIL suites must use separate,
explicitly authorized commands and must never be pulled into `Tests: All (no
flash)` merely by adding a `test_*` directory.

In VS Code, run `nRF54L15: Flash firmware` to build and upload the same
production image through `pio.ps1 upload`. This task requires a connected XIAO
nRF54L15 and is separate from the no-flash test tasks.
