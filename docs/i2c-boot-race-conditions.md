# I2C bus / boot ordering / AD7606 gating: race condition audit

Status: current as of 2026-09-18, reconciled against the live Linear
workspace (`Maxsokal` team). This document catalogs startup-ordering and
shared-resource race conditions between the ESP32-S3 firmware, the shared I2C0
sensor bus, and the main-enclosure XIAO nRF54L15 node.

Every item below is now filed in Linear (issue IDs inline). Items 2 and 3
turned out to already be resolved by prior work (MAX-28/MAX-29) that this
document's first draft missed because it read only `docs/adr-001-phase-e-i2c-bus.md`,
which is itself stale relative to Done Linear issues — see the note under
item 2/3. Do not re-open those from this doc; the ADR needs a refresh instead.

---

## 1. [FIXED in this session — MAX-112, Done] Shared I2C0 bus opened before keypad/display/SD

**Title:** Sequence sensor-bus I2C0 startup after keypad/display/SD, before Wi-Fi
**Linear:** [MAX-112](https://linear.app/maxsokal/issue/MAX-112) (child of MAX-108, comment added on MAX-108 explaining this does not close it)

**Description:** `main/app_startup.c` called `ina3221_monitor_start()` — which
lazily creates the shared I2C0 bus via `sensor_bus_init()` inside its worker
task — as the very first hardware component in
`app_startup_initialize_core()`, before `keypad_init()`, before
`app_spi_bus_init()`, and long before `display_init()` /
`storage_manager_init()`. The main-enclosure XIAO nRF54L15 is wired as an
upstream I2C *target* on this same physical bus (`docs/adr-001-phase-e-i2c-bus.md`,
`ARCHITECTURE.md:362-367`: XIAO `D4/SDA-0`/`D5/SCL-0` tied to ESP32-S3
GPIO9/GPIO10 via the TCA9548A harness). Probing that bus before the rest of
local hardware bring-up was interactive meant the earliest, least-recoverable
part of ESP32-S3 boot could contend with a still-booting nRF node's I2C target
on a bus with an already-documented unresolved address/instance-mapping gate
(ADR-001, "possible identical nRF target-address collision remains
unresolved").

**Fix applied:** `ina3221_monitor_start()` now runs inside
`app_startup_initialize_services()`, immediately after `storage_manager_init()`
completes and before the object-recovery/object-store/Wi-Fi stages. New order:
`keypad_init()` (core) → `display_init()` → `ui_boot_init()` →
`storage_manager_init()` → **`ina3221_monitor_start()`** →
`wifi_manager_init()` → `wifi_manager_start()`. Enforced by
`tests/python/test_i2c_boot_ordering_contract.py`.

**Important caveat — do not overclaim:** this only changes *when in the boot
sequence* the ESP32-S3 first touches I2C0; it does not add bus-recovery,
timeout hardening, or a handshake with the nRF node's readiness. The
previously observed raw `rst:0x1 (POWERON)` reset when the XIAO is physically
inserted is a full power-on reset signature, not a software watchdog/hang
signature, and the team's own prior investigation (`memory/2026-09-18.md`)
already concluded the production reset cause remains unconfirmed. The actual
tracked issue for this physical symptom is **MAX-108** ("MAIN XIAO insertion
prevents ESP32 startup"), with **MAX-109** covering the related `rst:0x1`
power-path isolation. **This fix is a defensive ordering improvement, not a
confirmed resolution of the physical hang/reset.** MAX-108's own HIL
hypothesis is GPIO back-power/regulator contention on `3V3_OUT`/`5V/VBUS`,
not I2C timing — treat MAX-108/MAX-109's HIL characterization as the
authoritative closure evidence, not this fix.

**Severity:** Medium (real ordering defect, fixed; root-cause of the reported
hang is still open under MAX-109).

**Files:** `main/app_startup.c`, `tests/python/test_i2c_boot_ordering_contract.py`.

---

## 2 & 3. [ALREADY RESOLVED — MAX-28, MAX-29, Done] Address collision + TWIS mapping

**This document's first draft got this wrong.** It read only
`docs/adr-001-phase-e-i2c-bus.md`, which still says the address collision
"remains unresolved" and the TWIS instance mapping "remains an implementation
gate." Both statements are **stale**: **MAX-28** ("define upstream I2C
discovery/addressing + Zephyr TWIS contract") closed 2026-09-15 with an
explicit DISCOVERY(0x30)/MAIN(0x31)/REMOTE(0x32) address-state model, a
one-hot TCA invariant enforced by the ESP32-side mux controller, and an
explicit correction that the pinned Zephyr v4.2.1 already has native TWIS
support via `i2c_nrfx_twis.c` — no HAL workaround needed. **MAX-29**
("Validate nRF TWIS foundation with pinned Windows PlatformIO/Zephyr build")
is also Done. `nRF54L15/include/nrf_upstream_protocol.h` already reflects the
MAX-28 addresses in code.

**Action taken:** no new Linear issues filed for these (would have
duplicated Done work). `docs/adr-001-phase-e-i2c-bus.md` still needs a text
refresh to stop asserting these as open — that's a documentation-hygiene
fix, not a code defect, and is not itself filed as a Linear issue.

---

## 4. [ALREADY TRACKED — MAX-108, Todo, Urgent] Power-rail/reset sequencing for the main-enclosure XIAO

**This document's first draft also got this wrong** — it proposed filing a
new issue for this. **MAX-108** ("MAIN XIAO insertion prevents ESP32 startup
— verify power path and unpowered-I2C behavior") already exists, already has
the correct hypothesis, and is already more precise than this document's
independent guess: it names the exact contract risk (XIAO `3V3_OUT` regulated
output vs `5V/VBUS` input, GPIO back-power on the shared D4/D5 -> GPIO9/GPIO10
lines if XIAO is unpowered while ESP32 drives them) and cites a real boot log
showing `ESP32_5V ≈ 0.176V` in the "successful" USB-only bench condition —
i.e. the normal 5V rail isn't even up in that control case. **MAX-109**
covers the related `rst:0x1 (POWERON)` isolation with a detailed Pololu
power-path protocol.

**Action taken:** added a comment to MAX-108 (see item 1) recording that
this session's software boot-reorder does not address its hypothesis. No new
issue filed — MAX-108 already is the correct, better-specified ticket.

---

## 5. [FIXED in this session — MAX-113, Done] AD7606 acquisition started unconditionally at boot

**Title:** Gate AD7606 acquisition start behind explicit user entry into the object/measurement menu
**Linear:** [MAX-113](https://linear.app/maxsokal/issue/MAX-113) (child of MAX-56, which serialized start/stop but assumed boot auto-start was correct — this issue changes that assumption)

**Description:** `seismic_acquisition_start()` used to run unconditionally
near the end of `app_startup_initialize_services()` (`main/app_startup.c`,
before `web_server_start()`/`wifi_manager_start()`), regardless of what the
user was doing on the local UI. This matches the requested product behavior
("AD7606 вообще запускать только после того как пользователь зашёл в меню
объекта").

**Fix applied:** `seismic_acquisition_init()` (hardware bring-up/presence
detection) still runs at boot in `app_startup_initialize_core()`, unchanged.
`seismic_acquisition_start()` (active sampling) no longer runs at boot at
all. It now starts from two call sites instead, both calling the existing
idempotent function so either order is safe:
- `components/ui/ui_screen_registry.c` `ui_screen_open_measurements()` — the
  local trigger, resolved to the **Measurements** screen (`UI_SCREEN_MEASUREMENTS`,
  home label literally `"MEASUREMENTS / AD7606"`) rather than the plain
  Objects list screen, since Objects is archive browsing with no live-ADC
  relevance and Measurements is where live channel data is actually rendered.
- `components/web_server/web_api_control.c` `web_control_measurement_start()`
  — a Web-only client that never opens the local UI must still be able to
  start a recording (`recording_start`/`measurement_start` actions), so
  acquisition is ensured before `measurement_service_start_object_async()` is
  queued. The existing explicit `acquisition_start` Web action is unchanged.

Enforced by `tests/python/test_seismic_acquisition_lifecycle_contract.py`
(`test_boot_initializes_lifecycle_but_does_not_auto_start_acquisition`,
`test_measurements_screen_entry_starts_acquisition`,
`test_web_measurement_start_ensures_acquisition_before_queuing`) and
`tests/python/test_startup_recovery_contract.py`.

**Files:** `main/app_startup.c`, `components/ui/ui_screen_registry.c`,
`components/web_server/web_api_control.c`,
`tests/python/test_seismic_acquisition_lifecycle_contract.py`,
`tests/python/test_startup_recovery_contract.py`.

---

## 6. [OPEN — MAX-114, Backlog] No gyroscope-calibration gate for AD7606 acquisition

**Title:** ESP32/nRF: surface REMOTE orientation/leveling status upstream and gate AD7606 start on all-connected-nodes-level
**Linear:** [MAX-114](https://linear.app/maxsokal/issue/MAX-114) (child of MAX-42)

**Correction from this document's first draft:** it's not quite "missing
entirely." **MAX-42** (Done) already implements the orientation/leveling
estimator on the nRF (`nRF54L15/src/nrf_orientation.c`), and **MAX-39**
(Done) already fully consumes the REMOTE sensor snapshot on the ESP32 side
inside `nrf_topology_service`. The actual, narrower gap — verified directly
against `nRF54L15/include/nrf_upstream_protocol.h` — is that the 102-byte
wire struct (`nrf_upstream_sensor_snapshot_t`) only carries raw
`accel_mg`/`gyro_mdps`/temperature/pressure/humidity; MAX-42's *computed*
ACCEPTED/OUTSIDE/etc. state never leaves the nRF. So despite two Done issues
in this exact area, the ESP32 still cannot know whether any REMOTE node is
level, and nothing gates `seismic_acquisition_start()` on it.

MAX-114 is filed as review-only (protocol extension + ESP32 aggregation +
acquisition gate + UI), matching this tracker's convention of listing open
design questions rather than guessing them, pending explicit approval before
implementation.

---

## 7. [OPEN — MAX-115, Backlog, Low] No concurrency test for INA3221 polling vs. nRF topology I2C reconciliation

**Title:** Tests: add host concurrency contract for sensor_bus shared between INA3221 and nRF topology tasks
**Linear:** [MAX-115](https://linear.app/maxsokal/issue/MAX-115) (child of MAX-14)

**Description:** `ina3221_monitor_worker` polls I2C0 every 1000 ms
indefinitely for the life of the device (`components/ina3221_monitor/ina3221_monitor.c:19,221-234`).
`nrf_topology_service` separately reconciles over the same bus every 5000 ms
(`NRF_TOPOLOGY_RECONCILE_PERIOD_MS`, confirmed by
`tests/python/test_nrf_topology_contract.py:48`). Both go through the
`espressif/i2c_bus` component's per-device mutex, so this is likely safe, but
no test — host or target — currently exercises concurrent access from two
independent tasks against `sensor_bus`. Given item 1's fix now starts INA3221
earlier relative to `nrf_topology_service` (which still starts after full
`app_startup_run()` per `main/main.c`) the temporal overlap window between the
two is unchanged by this session's fix, but remains unverified.

**Severity:** Low (no observed failure; verification gap only).

**Files:** `components/sensor_bus/`, `components/ina3221_monitor/`,
`components/nrf_topology_service/`, `managed_components/espressif__i2c_bus/`.

---

## Summary table — Linear ground truth (2026-09-18)

| # | Title | Linear | Status |
|---|---|---|---|
| 1 | Sequence sensor-bus I2C0 startup after keypad/display/SD, before Wi-Fi | [MAX-112](https://linear.app/maxsokal/issue/MAX-112) | Done (this session); comment added to MAX-108 |
| 2/3 | Address collision + TWIS mapping | [MAX-28](https://linear.app/maxsokal/issue/MAX-28), [MAX-29](https://linear.app/maxsokal/issue/MAX-29) | Already Done (2026-09-15); ADR-001 text is stale |
| 4 | MAIN XIAO insertion blocks ESP32 boot — power path | [MAX-108](https://linear.app/maxsokal/issue/MAX-108) | Todo, Urgent (pre-existing, already correctly scoped) |
| — | rst:0x1 POWERON isolation | [MAX-109](https://linear.app/maxsokal/issue/MAX-109) | Backlog, Urgent (pre-existing) |
| 5 | Gate AD7606 start behind Measurements-screen entry / Web request | [MAX-113](https://linear.app/maxsokal/issue/MAX-113) | Done (this session) |
| 6 | Surface REMOTE orientation/leveling status and gate AD7606 on it | [MAX-114](https://linear.app/maxsokal/issue/MAX-114) | Backlog, Medium (review-only, needs approval) |
| 7 | sensor_bus had zero locking around shared state — real bug, fixed | [MAX-115](https://linear.app/maxsokal/issue/MAX-115) | Done (this session) |

Two things this document's first draft got wrong before checking Linear:
items 2/3 were already closed (MAX-28/MAX-29), and item 4 already existed as
MAX-108 with a sharper, better-evidenced hypothesis than this document's
independent guess. Lesson: check Linear before writing up "open" findings in
a project that already tracks work there.

## 7 (revised). sensor_bus.c had zero locking around its own shared state

What MAX-115 was filed as ("likely safe by construction, verification gap
only") undersold it. Digging deeper into the actual `sensor_bus.c` source
(not just the `i2c_bus` per-device mutex it wraps) found a real,
unsynchronized, check-then-act race on `s_bus`/`s_ina3221`/`s_tca9548a`,
read and written from the INA3221 monitor task and the nRF topology service
task with no lock at all. Fixed with a FreeRTOS mutex
(`sensor_bus_lock_init()`, called once synchronously from
`app_startup_initialize_core()`), proven by a new real-multithreaded host
test (`tests/host/sensor_bus_concurrency_test.c`) that was verified to
actually fail when the lock is disabled and pass when it's enabled. See
MAX-115 for full detail. Real ESP-IDF 5.5.4 production build verified clean
(2506/2506) with this change.
