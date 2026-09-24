# TCA9548A shared-I2C contract

This document is the implementation contract for the ESP32-side TCA9548A foundation added by MAX-30. It deliberately stops before the nRF54L15 discovery/role-assignment wire protocol.

## Ownership

`components/sensor_bus` remains the only application owner of ESP32-S3 I2C0 on GPIO9/GPIO10 at 100 kHz. INA3221 and the optional TCA9548A are device handles on that one bus. No TCA or nRF component may call `i2c_bus_create()` or `i2c_bus_delete()` for I2C0.

The TCA handle returned by `sensor_bus_get_tca9548a()` is borrowed. Consumers must not delete it. `sensor_bus_deinit()` deletes the cached TCA handle before INA3221 and then deletes the shared bus.

## Targeted discovery

The physical A0/A1/A2 strap is not authoritative in the repository, so firmware does not assume one fixed TCA address. Lazy discovery probes only the TI-defined `0x70..0x77` range by creating a temporary device handle and reading the control byte with `NULL_I2C_MEM_ADDR`.

The result contract is:

- no responding address: `ESP_ERR_NOT_FOUND`; INA3221/shared-bus operation remains valid;
- exactly one responding address: retain and cache that device handle;
- more than one responding address: delete candidates and return `ESP_ERR_INVALID_STATE`;
- an I2C error other than a NACK: propagate the transport error instead of misreporting the TCA as absent.

A NACK is not one error code (MAX-248). The legacy driver reports it as `ESP_FAIL`, but the `i2c_bus` v2 backend used with ESP-IDF 5.5 returns `ESP_ERR_INVALID_STATE`, the same code as a hardware timeout. `sensor_bus_classify_transfer_error()` resolves that code with an address-only `i2c_master_probe()`: `ESP_ERR_NOT_FOUND` is a NACK (absent), an ACK means the target is present and the transfer failed, and a probe timeout is a bus fault. The nRF topology service uses the same classification, so a missing XIAO reads as absent rather than as a transport error.

There is intentionally no full `0x00..0x7F` scan during boot.

## One-hot mux invariant

`components/tca9548a_mux` is task-free. Its public API exposes only `close()` and `select(channel)`; there is no raw channel-mask function.

A verified state is either:

- `CLOSED`: control byte `0x00`, or
- `SELECTED`: exactly one bit set, `1 << channel` for channel `0..7`.

Before selecting any channel the component writes `0x00` and reads it back. It then writes the requested one-hot value and requires exact readback. A write/read/mismatch after selection triggers a best-effort verified close. If closure can be verified the state is `CLOSED`; otherwise it is `FAULT`. A failed close is never followed by opening another channel.

This software invariant is required because TCA9548A hardware itself permits multiple channels to be enabled simultaneously. The later shared nRF `REMOTE` address is safe only while the ESP32 controller enforces one-hot downstream visibility.

## RESET limitation

No TCA RESET GPIO is implemented in this slice because its physical wiring is not documented. Software therefore cannot claim hardware-reset recovery for a stuck downstream bus yet.

## Tests and local CI

`tests/host/manifest.d/coverage-tca.json` adds native C coverage for:

- close-before-select and exact one-hot readback;
- invalid-channel rejection without bus traffic;
- write/read/mismatch recovery and FAULT behavior;
- `i2c_bus` adapter use of the no-register control-byte transaction;
- zero, one, and multiple TCA discovery candidates;
- cached-handle ownership and cleanup order;
- propagation of non-NACK transport failures.

The repository verification path remains local (`tools/run_local_verify.py` / `tools/run_host_tests.py`). GitHub Actions are not part of this contract.

## Explicit non-goals

This slice does not define the nRF54L15 `DISCOVERY`, `MAIN`, or `REMOTE` numeric addresses in firmware, does not define assignment command framing, does not modify nRF `main.c`, and does not touch Bluetooth Channel Sounding.
