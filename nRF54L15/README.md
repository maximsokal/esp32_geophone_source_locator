# nRF54L15 firmware

Firmware for all Seeed Studio XIAO nRF54L15 nodes used by
esp32_geophone_source_locator.

## Architecture rule

There is ONE firmware implementation for every XIAO nRF54L15.

The firmware does not compile separate "central" and "remote" variants.

Runtime role will be discovered/assigned by the ESP32-S3 according to
the physical I2C topology:

- the XIAO visible while every TCA9548A downstream channel is disabled
  belongs to the main enclosure;
- XIAO devices that appear through individual TCA9548A channels belong
  to remote sensor nodes.

Remote hardware currently has status LEDs on:

- P1.04
- P1.05
- P1.06
- P1.07

The main-enclosure XIAO does not have these external LEDs fitted.

Future firmware stages will add:

- I2C target/TWIS protocol
- persistent node identity
- runtime role assignment
- BLE discovery
- Bluetooth Channel Sounding
- dynamic Channel Sounding initiator/reflector roles
- remote-node status LEDs

## Build

From the repository root:

    pio run -d .\nRF54L15 -e seeed-xiao-nrf54l15

Upload:

    pio run -d .\nRF54L15 -e seeed-xiao-nrf54l15 -t upload