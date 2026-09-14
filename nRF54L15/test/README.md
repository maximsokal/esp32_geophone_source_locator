# nRF54L15 PlatformIO test surface

The nRF subsystem already has executable hardware-free C coverage in the
repository root host-test harness:

```text
tests/host/manifest.d/coverage-nrf.json
```

That manifest currently exercises runtime node state, the private sensor-bus
contract, and role-aware aggregated sensor snapshots with the normal host C
compiler. `python tools/run_local_verify.py --scope nrf` runs this host policy
lane before the Zephyr production build.

This directory is reserved for future PlatformIO nRF54L15 test suites.

The automatic project-wide no-flash path is deliberately restricted to suites
named:

```text
test_sw_*
```

Only suites that have been explicitly qualified as hardware-free software/native
tests may use that prefix. `..\pio.ps1 test` and `Tests: All (no flash)` ignore
physical target/HIL suites.

PlatformIO's `--without-uploading` flag prevents firmware upload but does not by
itself guarantee that an embedded suite is hardware-free or that it will not
need a serial test port. For that reason board-dependent tests must use a
separate explicit future HIL runner/naming convention and must never be added
to `test_sw_*` merely to make them run from the master suite.

There is currently no executable PlatformIO `test_sw_*` suite, so that automatic
nRF test stage reports explicit `SKIPPED`. This does not mean there are no nRF
software tests: the host C lane above is executable and must remain green.

Future software/native suites must cover the remote sensor aggregator contract:
the external GY-BMI160 and BME280 share the private XIAO D11/D12 bus, are
reported through the remote nRF snapshot, and are not enumerated as raw devices
on the ESP32/TCA9548A bus. Physical sensor wiring, `3V3_OUT` power, address
straps and sensor reads belong to the HIL levels in
[`../../docs/nrf54l15-test-task.md`](../../docs/nrf54l15-test-task.md), not to a
software-only pass.

Do not create central/remote test firmware variants: all physical nodes use the
same production nRF firmware. A host/native-only test environment is permitted
if needed for software tests and does not count as a second production
firmware variant.
