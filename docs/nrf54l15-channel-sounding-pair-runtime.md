# nRF54L15 Channel Sounding directed-pair runtime (MAX-51)

MAX-51 turns the platform-neutral MAX-49 scheduler token into one real,
bounded NCS v3.4.0 / SoftDevice Controller Channel Sounding attempt. It does
**not** own the five-node schedule. The distributed handoff/control-plane that
decides which node receives the next `nrf_cs_step_t` remains MAX-97.

## Runtime boundary

Every XIAO still runs one common firmware image. Wired `MAIN` / `REMOTE`
assignment is independent from Bluetooth Central/Peripheral and Channel
Sounding Initiator/Reflector roles.

After the existing upstream ASSIGN succeeds, the nRF runtime publishes the
assigned `node_id` in RAS service data and advertises as an idle Ranging
Responder. When a caller asks this node to execute a scheduler step whose
`initiator` equals its assigned `node_id`, the runtime:

1. stops idle advertising;
2. scans for the requested reflector `node_id`;
3. creates the single BLE ACL connection;
4. raises the link to L2 security;
5. exchanges ATT MTU;
6. discovers RAS and allocates the RREQ client context;
7. reads peer RAS features and chooses real-time data or the documented
   on-demand fallback;
8. exchanges CS capabilities and creates one local+remote CS configuration;
9. enables CS security;
10. configures and enables exactly one CS procedure;
11. pairs local controller subevent data with peer RAS ranging data by ranging
    counter;
12. runs `cs_de_calc()`, maps an accepted `distance_estimates[0].best` value to
    integer millimetres, then disconnects and returns to idle advertising.

Incoming connections use the same image as a RAS RRSP / CS reflector. The RRSP
instance is auto-allocated for the one connection slot and reflector procedure
parameters are configured when the remote CS configuration is created.

## Bounded execution and stale protection

`BT_MAX_CONN=1` and `BT_CTLR_SDC_CS_COUNT=1` are deliberate. MAX-51 owns only
one directed pair attempt at a time.

The platform-neutral `nrf_cs_pair_adapter` keeps the complete MAX-49 step token
(`cycle_id`, membership generation, `attempt_id`, pair index/direction) through
the sequence. Events with a stale token cannot advance a newer attempt.
Completion is delayed until cleanup, so a caller cannot start the next pair
while the old ACL link is still being torn down.

The Zephyr runtime uses one delayable timeout derived from
`nrf_cs_step_t.timeout_ms`. It does not use `K_FOREVER` orchestration and it does
not reboot on an ordinary BLE disconnect. A timeout becomes
`NRF_CS_OUTCOME_TIMEOUT`; API/link failures become
`NRF_CS_OUTCOME_TRANSPORT_ERROR`; unusable/aborted ranging data becomes
`NRF_CS_OUTCOME_OUTLIER`.

## Distance quality

The initial provider confidence is intentionally **not a physical accuracy
probability**. `cs_de_calc()` exposes `CS_DE_QUALITY_OK` versus
`CS_DE_QUALITY_DO_NOT_USE`, so MAX-51 maps an accepted finite non-negative
estimate to confidence `1000` and an unusable estimate to `0`.

Provider flags separately record whether:

- real-time RAS was used;
- tone quality passed;
- a PBR estimate was finite;
- an RTT estimate was finite.

Real distance repeatability, bias and rubble/RF-environment accuracy remain a
separate HIL/calibration acceptance. A successful target build is not evidence
of centimetre-level ranging accuracy.

## Production Kconfig

Canonical `nRF54L15/zephyr/prj.conf` now permanently enables the reviewed
NCS v3.4.0 production stack:

- Central + Peripheral, `BT_MAX_CONN=1`;
- Nordic SoftDevice Controller multirole;
- Channel Sounding with both CS roles and one CS context;
- RAS RREQ + RRSP in the common image;
- RRSP automatic allocation for one active connection;
- one antenna path, mode 3 disabled;
- Nordic CS distance-estimation library with 512-point NFFT.

The MAX-45 `cs-proof` action remains a historical controller proof fixture. Its
overlay no longer owns production enablement and does not remove the permanent
RAS/CS_DE stack; it is a controller-configuration assertion layered on the same
common application rather than a second production firmware variant.

## Existing invariants

MAX-51 does not change:

- the 12-byte `SENSOR_REFRESH` frame;
- the 112-byte REMOTE sensor snapshot;
- ESP32 topology/TCA behavior;
- BMI160/BME280 sampling;
- geophone acquisition or TDOA;
- WebXR/phone anchoring;
- multi-node execution authority (MAX-97);
- physical ranging accuracy/HIL claims.

The next architectural layer is MAX-97: distribute each MAX-49 directed step
to the correct initiator node and collect the returned measurement while still
respecting the one-connection-per-node constraint.
