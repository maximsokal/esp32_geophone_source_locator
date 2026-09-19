# ESP32 nRF REMOTE sensor consumer

This slice extends the existing ESP32 `nrf_topology_service` to consume the
MAX-36 REMOTE sensor snapshot without creating a second owner of the shared
I2C0/TCA9548A path.

## Ownership

The existing `nrf_topology` task remains the only application-level owner of
TCA channel selection and the 0x30/0x31/0x32 nRF device handles. Public sensor
APIs only set pending bits and wake that task; callers never receive bus, TCA,
or nRF device handles.

## Assignment epoch

Each topology node snapshot now retains the bootstrap STATUS `request_id` as
`assignment_request_id`. The sensor-cache identity is hardware ID, node ID,
mux channel, runtime address, and this assignment epoch. A normal topology
`generation` increment does not invalidate a sample. A new assignment epoch
does, even when the same physical nRF returns on the same channel after a
reboot.

## Sensor transaction

For a topology-confirmed REMOTE channel the pure transport controller performs:

1. verified one-hot TCA select;
2. optional MAIN 0x31 identity/epoch validation when MAIN is assigned;
3. REMOTE 0x32 identity/epoch validation;
4. one 12-byte `SENSOR_REFRESH` write with a separate non-zero ESP32 request ID;
5. bounded polling of the canonical 112-byte REMOTE frame;
6. acceptance only of the expected request ID with a final result;
7. ESP32 local receipt timestamp capture;
8. verified TCA close on every selected-channel exit path.

The sensor transaction never accesses discovery address 0x30. The total
default deadline is 500 ms with 10 ms polling. A foreign PENDING context (for
example the nRF assignment-triggered request 0) is allowed to finish. If a
foreign final context is seen, the ESP32 resends its same request ID once.
Expected BUSY is terminal for that request ID.

A close error dominates the transaction result. If B1 reports that the mux is
actually in FAULT, the cache state becomes `MUX_FAULT`; the last decoded sensor
frame may remain for diagnostics but is not topology-current.

## Cache semantics

There is one cache entry per REMOTE channel. Clean absence or a changed
identity/assignment epoch clears old payload. Transient topology
conflict/protocol/transport states retain last-known data only for diagnostics
and mark it not current. Sensor identity mismatch, remote NACK/reset, MAIN
validation failure, and mux-close fault request an immediate topology
reconcile.

The nRF `sample_complete_uptime_ms` is preserved unchanged. ESP32 also stores a
local monotonic receipt timestamp. These clocks are not synchronized and are
not geophone/TDOA timestamps.

## Scheduling

Sensor work uses an explicit 8-bit pending mask plus an in-flight mask. Duplicate
requests coalesce. The worker keeps an elapsed-tick 5 s topology deadline;
sensor notifications are wakeups only and do not restart that deadline. If the
deadline becomes due between channels, remaining sensor bits are requeued and
topology reconciliation runs first. A single in-progress sensor transaction can
delay the deadline only by its bounded transaction time.

A newly discovered or reassigned REMOTE receives one automatic ESP-side initial
refresh. There is no continuous periodic sensor polling in this slice.

## Validation boundary

Host tests cover cache epoch semantics, all eight channels, MAIN/REMOTE identity
validation, the foreign-request retry rule, final sensor results, timeout,
NACK/protocol errors, abort, and close-fault dominance. Static contracts check
single-task/single-owner scheduling and canonical codec use.

Exact Windows ESP-IDF target-build evidence and real XIAO/TCA early-STOP/full
112-byte behavior remain separate validation gates.
