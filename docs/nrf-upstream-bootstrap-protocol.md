# nRF54L15 upstream bootstrap protocol v1

This document is the byte-level contract for assigning a runtime identity to a
XIAO nRF54L15 before the later sensor-snapshot and Channel Sounding protocols
exist. It is shared by every production nRF image and the future ESP32
controller implementation.

## Address map

All addresses are 7-bit I2C target addresses:

| Purpose | Address |
| --- | ---: |
| Unassigned discovery target | `0x30` |
| Assigned main-enclosure target | `0x31` |
| Assigned remote target | `0x32` |
| TCA9548A reserved range | `0x70..0x77` |

Every reset is intentionally ephemeral: the node returns to UNASSIGNED at
`0x30`. No assignment is persisted to flash in protocol v1.

## Common header

Every v1 frame starts with ten bytes:

| Offset | Field |
| ---: | --- |
| 0..1 | magic `47 53` (`GS`) |
| 2 | protocol version `1` |
| 3 | message type |
| 4..5 | total frame length, little-endian |
| 6..9 | request ID, little-endian `uint32_t` |

Request ID zero is reserved for the initial/no-request STATUS. ASSIGN requests
must use a nonzero request ID.

Frames end in CRC-16/CCITT-FALSE (`poly=0x1021`, `init=0xFFFF`, refin=false,
refout=false, xorout=0). The CRC is calculated over all preceding frame bytes
and stored little-endian. Serialization is explicit; packed C structs are not
part of the wire ABI.

## ASSIGN (`type 0x01`, 24 bytes)

| Offset | Field |
| ---: | --- |
| 0..9 | common header |
| 10 | expected hardware-ID length, `1..8` |
| 11..18 | expected hardware ID, zero-padded to 8 bytes |
| 19 | role: `1=MAIN`, `2=REMOTE` |
| 20 | node ID `0..254`; `0xFF` is reserved |
| 21 | mux channel: `0xFF` for MAIN, `0..7` for REMOTE |
| 22..23 | CRC16 |

The hardware ID must exactly match the boot-time `hwinfo_get_device_id()`
bytes. The role derives the target address; the controller cannot command an
arbitrary target address.

## STATUS (`type 0x80`, 28 bytes)

A pure I2C read returns STATUS at the node's currently registered address.
There is no GET_INFO write in v1.

| Offset | Field |
| ---: | --- |
| 0..9 | common header; request ID is the latest request context |
| 10 | hardware-ID length |
| 11..18 | hardware ID, zero-padded |
| 19 | role: `0=UNASSIGNED`, `1=MAIN`, `2=REMOTE` |
| 20 | node ID; `0xFF` while unassigned |
| 21 | mux channel; `0xFF` while unassigned or MAIN |
| 22 | currently registered target address, or zero if unregistered |
| 23 | lifecycle: `0=BOOTING`, `1=READY`, `2=UNAVAILABLE` |
| 24 | stable protocol result code |
| 25 | flags: bit0 ASSIGNED, bit1 TARGET_REGISTERED |
| 26..27 | CRC16 |

The stable result codes are `OK`, `PENDING`, `INVALID_FRAME`, `BAD_CRC`,
`UNSUPPORTED_VERSION`, `UNSUPPORTED_TYPE`, `REQUEST_ID_INVALID`,
`HW_ID_MISMATCH`, `ROLE_INVALID`, `NODE_ID_INVALID`, `CHANNEL_INVALID`,
`ROLE_CONFLICT`, `REQUEST_CONFLICT`, `BUSY`, `READDRESS_FAILED`, and
`INTERNAL_ERROR`. Internal errno values and `nrf_node_state_result_t` values are
never serialized directly.

## Deferred assignment and recovery

The pinned Zephyr 4.2.1 Nordic TWIS buffer callback runs on the nrfx event
path. The callback therefore only decodes/copies a complete ASSIGN, places it
in a bounded non-blocking queue, and submits work. Target unregister/register
and identity commit occur in worker context.

The worker publishes PENDING, validates the hardware identity and request
semantics, derives `0x31` or `0x32` from the role, performs re-addressing, and
publishes the final STATUS. An I2C write ACK only proves delivery. The future
ESP32 orchestrator must confirm the same hardware ID, request ID, role/node/
channel tuple, and `OK` result by reading STATUS at the expected new address.

Repeated identical assignment is idempotent. Reusing one request ID for a
different payload is `REQUEST_CONFLICT`. A transient re-address failure that
successfully rolls back to `0x30` returns `READDRESS_FAILED` and may be retried
with the same request. If both the new registration and rollback fail, the node
cold-reboots from deferred context so it can return to the normal discovery
address instead of remaining unreachable.

Protocol v1 does not start the REMOTE BMI160/BME280 runtime, persist roles,
transport sensor snapshots, or implement Bluetooth Channel Sounding.
