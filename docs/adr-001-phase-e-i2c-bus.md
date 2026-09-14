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

There is no `sensor_hub` component in this phase. Revisit it only when real
BMI160/BMP580 drivers and four pod devices exist, and the TCA9548A address,
channel mapping, and electrical contract are documented. This checkout has no
TCA9548A address, so no mux device handle is registered here.

This ADR describes the current ESP32-S3 I2C0 ownership boundary. The planned
nRF54L15 topology in [`ARCHITECTURE.md`](../ARCHITECTURE.md) does not transfer
that ownership to an nRF node: ESP32-S3 remains the wired-I2C controller, the
main nRF target is upstream of the TCA9548A, and remote targets are downstream
channel devices. A remote BME280 is separately powered from 3.3 V and is not
an nRF54L15 power output or an assumed nRF I2C-master workload. The possible
identical nRF target-address collision remains unresolved and must be settled
before an I2C/TWIS protocol or a corresponding test fixture is implemented.
The BMI160/BMP580 names in this phase's existing ESP-side sensor-hub watchlist
do not identify the XIAO nRF54L15 Sense IMU; that board fact is the separate
Seeed-documented `LSM6DS3TR-C` contract in `ARCHITECTURE.md`.

## Risks

The implementation uses the `i2c_bus` 1.x C handle API rather than calling the
native ESP-IDF master-bus API from the monitor. A future component/API change
must be validated by an ESP-IDF build and HIL. The vendor component's default
mutex wait policy is now the transfer timeout boundary rather than the old
driver's explicit 100 ms argument. The manifest and `dependencies.lock` are
updated together and pin `espressif/i2c_bus` at 1.5.2 for this migration.
