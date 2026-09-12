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

## Risks

The implementation uses the `i2c_bus` 1.x C handle API rather than calling the
native ESP-IDF master-bus API from the monitor. A future component/API change
must be validated by an ESP-IDF build and HIL. The vendor component's default
mutex wait policy is now the transfer timeout boundary rather than the old
driver's explicit 100 ms argument. The manifest and `dependencies.lock` are
updated together and pin `espressif/i2c_bus` at 1.5.2 for this migration.
