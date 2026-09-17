# nRF54L15 NCS Channel Sounding build proof

This document preserves the MAX-45 **build-only** Channel Sounding proof. It is
historical/reproducible controller evidence, not physical ranging evidence and
not a second production firmware.

## Baseline

The proof is pinned to **nRF Connect SDK v3.4.0**. Its Zephyr tree contains the
upstream board target `xiao_nrf54l15/nrf54l15/cpuapp`; no separate Seeed board
root is required.

The XIAO NCS board definition owns the board RF switch. The project does not add
application-owned RF-switch GPIO toggling and does not enable multi-antenna
Channel Sounding.

The existing `nRF54L15/zephyr/app.overlay` remains authoritative for project
I2C topology:

- upstream D4/P1.10 SDA + D5/P1.11 SCL: `i2c22` as `nordic,nrf-twis`;
- private D12/P0.04 SDA + D11/P0.03 SCL: `i2c30` remains controller/TWIM;
- board-declared Sense IMU stays disabled because the diploma uses the plain
  XIAO plus external BMI160 on remote nodes.

## Why NCS is the production authority

The historical PlatformIO baseline uses Zephyr 4.2.1. MAX-45 proved the actual
nRF54L15 Channel Sounding controller path with Nordic SoftDevice Controller in
NCS v3.4.0 / Zephyr 4.4.0. MAX-96 therefore promoted NCS v3.4.0 to the
canonical production/flash toolchain.

MAX-51 subsequently promoted the reviewed RAS requester/responder, Channel
Sounding and CS distance-estimation options into canonical
`nRF54L15/zephyr/prj.conf`. PlatformIO remains compatibility-smoke only.

## MAX-45 proof configuration after MAX-51

`nRF54L15/ncs/channel_sounding.conf` is still used by the `cs-proof` action,
but after MAX-51 it is intentionally an **override fixture** rather than the
place where production Channel Sounding is enabled.

Production already contains Channel Sounding/RAS/CS_DE. The proof overlay keeps
the original controller shape but does **not** remove the permanent production
RAS/CS_DE stack; it is now a controller/Kconfig assertion layered on the same
common application source and production configuration.

The proof keeps both Bluetooth link roles and both CS roles while retaining one
simultaneous connection/context:

- `CONFIG_BT_CENTRAL=y`
- `CONFIG_BT_PERIPHERAL=y`
- `CONFIG_BT_MAX_CONN=1`
- `CONFIG_BT_CTLR_SDC_PERIPHERAL_COUNT=1`
- `CONFIG_BT_CTLR_SDC_CS_ROLE_BOTH=y`
- `CONFIG_BT_CTLR_SDC_CS_COUNT=1`

It is single-antenna, step-mode-3 disabled and Channel-Sounding test commands
are disabled.

The historical MAX-45 resource numbers below are frozen evidence from the
original proof commit. A current `cs-proof` build is not expected to reproduce
byte-for-byte size because the common application source tree has evolved.

## Windows execution

Canonical production build:

```powershell
.\nRF54L15\ncs.ps1 build
```

Re-run the controller proof:

```powershell
.\nRF54L15\ncs.ps1 cs-proof
```

Build production and proof back-to-back and print sizes:

```powershell
.\nRF54L15\ncs.ps1 compare
```

Inspect an existing proof artifact:

```powershell
.\nRF54L15\ncs.ps1 probe
```

Cleanup:

```powershell
.\nRF54L15\ncs.ps1 clean
```

The wrapper resolves/pins NCS v3.4.0, requires the upstream XIAO board, uses a
no-space Windows source alias where needed, and stores generated build files
outside the checkout.

## Historical verified evidence

On 2026-09-17, the MAX-45/MAX-49 gate built successfully with NCS v3.4.0:

- `MAX-45 KCONFIG_PROBE: PASS`;
- linked Channel Sounding proof image;
- FLASH **216244 B / 1524 KB (13.86%)**;
- RAM **43800 B / 256 KB (16.71%)**;
- wrapper exit `NCS BUILD = 0`.

Those numbers are build-resource evidence, not physical-ranging accuracy.

## Invariants

The proof/toolchain path does not change:

- one-image runtime MAIN/REMOTE model;
- 12-byte `SENSOR_REFRESH`;
- 102-byte REMOTE snapshot;
- ESP32 topology/sensor consumer;
- BMI160/BME280/orientation logic;
- geophone acquisition/TDOA;
- physical LED wiring;
- cross-node clock synchronization.

MAX-49 supplies the platform-neutral scheduler. MAX-51 supplies the reviewed
one-directed-pair NCS RREQ/RRSP execution runtime. Multi-node execution
authority/handoff remains MAX-97 so `BT_MAX_CONN=1` does not become an implicit
distributed BLE race.
