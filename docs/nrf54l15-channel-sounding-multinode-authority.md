# MAX-97: Channel Sounding multi-node execution authority

MAX-97 makes the ESP32-S3 the **single global Channel Sounding cycle authority**. The nRF54L15 nodes remain pair executors: a node may become CS initiator or reflector for a directed step, but no nRF independently advances the five-node schedule. This avoids split brain while preserving the common nRF firmware image.

## Ownership

- `nrf_topology_service` remains the only owner of the TCA9548A transaction path.
- The same topology worker hosts the platform-neutral `nrf_cs_authority` state machine; MAX-97 does **not** create a second TCA task.
- `nrf_cs_authority` reuses the canonical MAX-49 `nrf_cs_scheduler` implementation shared from `nRF54L15/src`.
- MAX-51 remains the NCS/RAS pair executor. `BT_MAX_CONN=1` is unchanged.
- Wired MAIN/REMOTE role never selects the CS role. Any connected `node_id` can receive an initiator turn.

## Versioned I2C control plane

The legacy frames remain byte-for-byte unchanged:

- ASSIGN: 24 bytes
- STATUS: 28 bytes
- SENSOR_REFRESH: 12 bytes
- REMOTE snapshot: 102 bytes

MAX-97 adds separate CS messages under the existing protocol version and CRC discipline:

- `CS_EXECUTE`: 32 bytes
- `CS_STATUS_REQUEST`: 32 bytes
- `CS_RESULT`: 44 bytes

`CS_EXECUTE` and `CS_STATUS_REQUEST` carry `request_id` plus the complete MAX-49 directed-step token: `cycle_id`, `membership_generation`, `attempt_id`, `pair_index`, `attempt_number`, `initiator`, `reflector`, and `timeout_ms`.

A `CS_STATUS_REQUEST` arms a one-shot result selector. The next read is always a 44-byte `CS_RESULT` for that selector, including explicit RUNNING, COMPLETE/FAILED, BUSY, INVALID, or STALE state. After that one read, normal STATUS/REMOTE-snapshot behavior resumes.

## Handoff and REMOTE-to-REMOTE execution

For a scheduled `node 3 -> node 5` step, the ESP32 selects the TCA channel that owns node 3 and sends `CS_EXECUTE` only to node 3. Node 3 uses the MAX-51 BLE/RAS runtime to find node 5 by stable advertised `node_id` and performs the directed Channel Sounding attempt. Node 5 participates as the reflector through the same common nRF image. The ESP32 then polls node 3 over I2C for the terminal result.

This means remote-to-remote pairs do not require the MAIN nRF to be a permanent BLE master and do not require more than one BLE connection per nRF.

## Recovery rules

- A lost I2C acknowledgement may repeat the exact same `CS_EXECUTE`. The nRF control runtime treats that request id + full token as idempotent and does not launch a second RF attempt.
- A different request received while an attempt is active returns BUSY.
- Results are accepted by ESP32 only when `request_id` and the complete MAX-49 token match the active command.
- Attempt timeout is bounded by `step.timeout_ms`; timeout is fed back into the MAX-49 retry policy.
- If topology reconciliation reports a node gone, the ESP32 authority calls the MAX-49 disconnect path. A node that returns is eligible only when the next cycle starts, matching the scheduler's membership-generation semantics.
- TCA routes are closed after each CS transaction. A transport/close failure requests topology reconciliation rather than introducing another bus owner.

## Verification boundary

Host tests cover the 32/32/44-byte codec, CRC rejection, nRF idempotent/BUSY/STALE behavior, five-node ten-pair execution, fair initiator handoff, lost acknowledgement, timeout/retry, disconnect, and rejoin on the next cycle. ESP-IDF and canonical NCS v3.4.0 builds verify integration on both firmware sides.

Actual RF ranging behavior and accuracy remain **HIL-only**. MAX-97 does not claim physical Channel Sounding accuracy from host/build evidence.
