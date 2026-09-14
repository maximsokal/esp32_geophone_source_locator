# ESP32 nRF topology service

This document freezes the ESP32-side bootstrap topology contract introduced by
Slice B3. It complements `nrf-upstream-bootstrap-protocol.md`; it does not
replace the canonical wire codec in `nRF54L15`.

## Ownership

`sensor_bus` remains the sole creator/deleter of ESP32 I2C0 on GPIO9/GPIO10.
`nrf_topology_service` borrows that bus and the optional sensor_bus-owned
TCA9548A handle. It owns only three logical I2C device handles for the nRF
bootstrap addresses `0x30`, `0x31`, and `0x32` and deletes those handles before
safe shutdown reaches `sensor_bus_deinit()`.

The topology service is the sole application-level owner of TCA channel
selection. A future remote sensor snapshot transport must use this same broker;
it must not select TCA channels independently, because thread-safe individual
I2C operations do not make `select -> remote transaction -> close` atomic.

## Collision-avoidance order

At reset every unassigned nRF uses discovery address `0x30`. Therefore the
service always reconciles the direct MAIN node with the TCA verified closed
before it opens any remote channel. MAIN is assigned logical node ID 0 and
runtime address `0x31`.

Only after MAIN is either assigned safely or proven absent may the service scan
TCA channels 0 through 7. A remote on channel `N` receives logical node ID
`N + 1`, mux channel `N`, and runtime address `0x32`. The mux is verified closed
after every channel, including error paths.

When a known MAIN disappears during a selected-channel window, the service
closes the mux and returns to direct MAIN reconciliation before any later read
of discovery address `0x30`.

## Warm reset and assignment confirmation

An ESP32 reset does not require nRF resets. Existing MAIN/REMOTE assignments are
accepted only when STATUS has the expected role, physical slot, runtime address,
hardware ID, READY lifecycle, registered/assigned flags and protocol result.
A stale moved remote is a conflict rather than silently receiving a new slot ID.
Duplicate hardware IDs in the same topology snapshot are conflicts.

ASSIGN write ACK is delivery evidence only. The controller confirms the exact
hardware ID, request ID, role/node/channel tuple and role-derived runtime
address. Default confirmation timeout is 500 ms with a 10 ms poll interval.
`READDRESS_FAILED` permits one same-request retry. If a target cold-reboots
back to discovery before confirmation, the bounded confirmation attempt expires;
the next periodic reconcile starts a fresh assignment request from a clean
discovery snapshot.

## Runtime lifecycle

The service starts only after `app_startup_run()` has completed, so TFT/keypad
interactivity and `startup_total` are not extended by topology discovery. One
low-priority Core-0 task performs an initial pass, repeats every 5 seconds and
can be explicitly refreshed. Its in-RAM snapshot is copied under a narrow
critical section.

Shutdown signals the topology task first. Assignment polling checks the stop
request between bounded polls, the worker closes the mux, deletes its three nRF
address handles, exits, and only then may the application stop INA3221 and
deinitialize the shared sensor bus.

## Explicit non-goals

Slice B3 does not transport BMI160/BME280 samples, start the remote sensor
runtime on role assignment, expose topology in the TFT/Web UI, persist role
assignments, add a TCA RESET GPIO, or implement Bluetooth Channel Sounding.
