# nRF54L15 upstream REMOTE sensor snapshot protocol

This document extends the bootstrap contract in
`docs/nrf-upstream-bootstrap-protocol.md`. It does not replace ASSIGN/STATUS
semantics and does not introduce a second production firmware image.

## Runtime boundary

All XIAO nRF54L15 boards still boot UNASSIGNED and use the same bootstrap
firmware. Only a successfully assigned REMOTE node activates its private
D11/D12 (`i2c30`) GY-BMI160/BME280 runtime. MAIN and UNASSIGNED nodes perform
no private sensor transactions.

The bootstrap worker never performs sensor I/O. After a REMOTE assignment is
committed, it queues an immutable copy of the assignment to a dedicated sensor
thread. BME280 forced conversion sleeps and private I2C transfers therefore do
not block the TWIS callback or Zephyr system workqueue.

## TWIS buffer

The pinned Zephyr 4.2.1 nrfx TWIS buffer is configured explicitly:

```text
CONFIG_I2C_NRFX_TWIS_BUF_SIZE=128
```

The reviewed full REMOTE frame is 112 bytes. i2c22 remains the only upstream
TWIS instance; i2c30 remains the private TWIM/controller.

## SENSOR_REFRESH request

Fixed length: 12 bytes.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 2 | common magic `47 53` (`GS`) |
| 2 | 1 | protocol version `1` |
| 3 | 1 | message type `0x02` |
| 4 | 2 | total length `12`, little-endian |
| 6 | 4 | nonzero request ID, little-endian |
| 10 | 2 | CRC-16/CCITT-FALSE over bytes 0..9, little-endian |

Only one explicit sensor request may be outstanding per REMOTE node. Repeating
the same request ID is idempotent. A different request ID received while one
conversion is pending is not queued and does not replace the active published
request context.

## Backward-compatible REMOTE read

A REMOTE read publishes a 112-byte frame. The first 28 bytes are exactly the
existing STATUS frame, including the existing STATUS CRC at bytes 26..27.
Existing topology code can therefore continue to clock only 28 bytes and use
the unchanged STATUS decoder.

MAIN and UNASSIGNED nodes continue to publish only the 28-byte STATUS frame.

### Sensor extension

| Offset | Size | Field |
| ---: | ---: | --- |
| 28 | 2 | extension magic `53 4E` (`SN`) |
| 30 | 1 | extension version `1` |
| 31 | 1 | extension type `1` (sensor snapshot) |
| 32 | 2 | full frame length `112`, LE |
| 34 | 4 | sensor request ID, LE; `0` means assignment-triggered initial sample |
| 38 | 4 | snapshot sequence, LE |
| 42 | 4 | valid mask, LE |
| 46 | 4 | fault mask, LE |
| 50 | 8 | sample-complete nRF uptime in ms, LE |
| 58 | 1 | sensor result |
| 59 | 1 | sensor flags |
| 60 | 12 | accel X/Y/Z, signed i32 mg, LE |
| 72 | 12 | gyro X/Y/Z, signed i32 mdps, LE |
| 84 | 4 | IMU temperature, signed i32 milli-C, LE |
| 88 | 4 | environmental temperature, signed i32 milli-C, LE |
| 92 | 4 | pressure, u32 Pa, LE |
| 96 | 4 | humidity, u32 milli-percent, LE |
| 100 | 4 | altitude, signed i32 mm, LE |
| 104 | 1 | orientation state, wire value 0..3 |
| 105 | 1 | orientation blink class, wire value 0..3 |
| 106 | 4 | tilt sin², u32 parts per million, LE |
| 110 | 2 | CRC-16/CCITT-FALSE over bytes 0..109, LE |

The full CRC includes the nested STATUS prefix so the sensor payload is bound
to the identity/role/node/channel/status context returned in the same read.
The nested STATUS CRC remains independently valid.

## Sensor result values

| Value | Meaning |
| ---: | --- |
| 0 | OK |
| 1 | PENDING |
| 2 | PARTIAL |
| 3 | NOT_READY |
| 4 | NOT_REMOTE |
| 5 | NO_DATA |
| 6 | BUSY |
| 7 | INTERNAL_ERROR |

These are wire values. They do not expose Bosch driver errors, Zephyr errno,
or internal runtime enum values.

## Sensor flags and masks

Flags:

- bit 0: prepare attempted;
- bit 1: IMU ready;
- bit 2: environment sensor ready;
- bit 3: sample available;
- all remaining bits are reserved and must be zero in v1.

The valid/fault masks retain the existing aggregate snapshot meanings:

- bit 0: IMU;
- bit 1: environment sensor.

A valid and fault bit for the same source must not be set simultaneously.

## Freshness and timing

`sample_complete_uptime_ms` is local monotonic nRF uptime captured after an
actual aggregate refresh attempt completes. It is freshness/debug metadata,
not a synchronized clock. It must not be used for cross-node timing or
geophone TDOA. BMI160 and BME280 reads are sequential and are not claimed to
be simultaneous.

## HIL gate

Software tests cannot prove target-controller early STOP behavior. Before this
transport is treated as physically validated, the real XIAO/TCA harness must
show that:

1. ESP32 can clock only the 28-byte STATUS prefix from a REMOTE that prepared a
   112-byte TX buffer, issue STOP, and perform another clean transaction;
2. a complete 112-byte read passes both nested STATUS CRC and full-frame CRC;
3. real GY-BMI160/BME280 initialization/read timing works on D11/D12;
4. a MAIN board with no remote sensor assembly performs no private sensor I/O.
