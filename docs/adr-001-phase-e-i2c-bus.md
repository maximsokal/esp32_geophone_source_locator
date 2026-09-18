# ADR-001: Phase E shared sensor I2C bus

## Decision

`components/sensor_bus` is the sole application owner of I2C0. It uses
`espressif/i2c_bus` 1.5.2, keeps SDA GPIO9, SCL GPIO10, the existing internal
pull-up setting, INA3221 address `0x40`, and 100 kHz. It exposes borrowed bus
and INA3221 device handles. `ina3221_monitor` uses those handles and never
creates, adds, removes, or deletes the shared bus/device.

`i2c_bus` 1.5.2 maps this configuration to the ESP-IDF master-bus API and
uses the existing native glitch filter value of 7. Its thread-safe device
operations retain the monitor's register transactions without a second
application-level bus owner.

## Watchlist: sensor_hub

There is no `sensor_hub` component in this phase. Revisit it only when the
actual ESP-side pod devices exist and the TCA9548A address, channel mapping,
and electrical contract are documented. This checkout has no TCA9548A address,
so no mux device handle is registered here.

This ADR describes the current ESP32-S3 I2C0 ownership boundary. The planned
nRF54L15 topology in [`ARCHITECTURE.md`](../ARCHITECTURE.md) does not transfer
upstream bus ownership to an nRF node: ESP32-S3 remains the wired-I2C
controller, the main nRF target is upstream of the TCA9548A, and remote nRF
targets are downstream channel devices. Each remote nRF also owns a private
local sensor bus on D11/P0.03 and D12/P0.04. The local bus contains that
remote node's GY-BMI160 and BME280; neither sensor is directly enumerated on
ESP32-S3 I2C0/TCA9548A. Both sensor modules are planned to receive board power
from the XIAO `3V3_OUT` rail, while the RJ45 3.3 V conductor (pin 2) remains
RESERVED/NC.

The nRF target-address collision and TWIS instance-mapping gate flagged in an
earlier draft of this ADR are resolved: Linear MAX-28 defines the
DISCOVERY(0x30)/MAIN(0x31)/REMOTE(0x32) address-state model with a one-hot
TCA invariant, `nRF54L15/include/nrf_upstream_protocol.h` implements it, and
MAX-28 also corrects an earlier assumption here — the pinned Zephyr v4.2.1
already provides native TWIS support through `i2c_nrfx_twis.c`; no custom HAL
workaround is required. MAX-29 validated the TWIS foundation build. Remaining
open work in this area (main-enclosure XIAO power/reset sequencing when
physically inserted) is tracked as Linear MAX-108/MAX-109, not here.

The remote geophone orientation IMU is now explicitly an external **GY-BMI160**
connected locally to each plain XIAO nRF54L15 on the XIAO's second I2C pair
(`D11/P0.03` SCL and `D12/P0.04` SDA). The BME280 is on that same local bus and
is aggregated by the remote nRF. Neither sensor is a device on ESP32-S3 I2C0
and neither may be added to `sensor_bus` merely because it is an I2C peripheral.
The installed XIAO boards are the plain nRF54L15 variant and have no built-in
Sense IMU.

## Risks

The implementation uses the `i2c_bus` 1.x C handle API rather than calling the
native ESP-IDF master-bus API from the monitor. A future component/API change
must be validated by an ESP-IDF build and HIL. The vendor component's default
mutex wait policy is now the transfer timeout boundary rather than the old
driver's explicit 100 ms argument. The manifest and `dependencies.lock` are
updated together and pin `espressif/i2c_bus` at 1.5.2 for this migration.
