# nRF54L15 NCS Channel Sounding build proof

This document preserves the MAX-45 **build-only** Channel Sounding proof. It is
historical/reproducible evidence, not the current production configuration and
not physical ranging evidence.

## Baseline

The proof is pinned to **nRF Connect SDK v3.4.0**. Its Zephyr tree contains the
upstream board target `xiao_nrf54l15/nrf54l15/cpuapp`; no separate Seeed board
root is required for this path.

The XIAO NCS board definition owns the board RF switch. The project does not add
application-owned RF-switch GPIO toggling and does not enable multi-antenna
Channel Sounding in this proof.

The existing `nRF54L15/zephyr/app.overlay` remains authoritative for project
I2C topology:

- upstream D4/P1.10 SDA + D5/P1.11 SCL: `i2c22` as `nordic,nrf-twis`;
- private D12/P0.04 SDA + D11/P0.03 SCL: `i2c30` remains controller/TWIM;
- board-declared Sense IMU disabled because the diploma uses the plain XIAO plus
  external BMI160 on remote nodes.

## Why NCS became the production authority

The historical PlatformIO baseline uses Zephyr 4.2.1. MAX-45 proved the actual
nRF54L15 Channel Sounding controller path with Nordic SoftDevice Controller in
NCS v3.4.0 / Zephyr 4.4.0. MAX-96 therefore promoted NCS v3.4.0 to the
canonical production/flash toolchain instead of maintaining two different
things called “production firmware.”

The PlatformIO path is now compatibility-smoke only.

## Proof configuration

`nRF54L15/ncs/channel_sounding.conf` remains additive to
`nRF54L15/zephyr/prj.conf`. It is intentionally **not** included by canonical
`ncs.ps1 build` until the MAX-51 RAS/Channel-Sounding adapter promotes the
reviewed permanent options.

The MAX-45 proof common image enables both Bluetooth connection roles and both
CS roles while retaining one simultaneous connection/context:

- `CONFIG_BT_CENTRAL=y`
- `CONFIG_BT_PERIPHERAL=y`
- `CONFIG_BT_MAX_CONN=1`
- `CONFIG_BT_CTLR_SDC_PERIPHERAL_COUNT=1`
- `CONFIG_BT_CTLR_SDC_CS_ROLE_BOTH=y`
- `CONFIG_BT_CTLR_SDC_CS_COUNT=1`

It is single-antenna, step-mode-3 disabled, Channel-Sounding test commands
disabled, and uses one result reassembly buffer. MAX-45 does not enable RAS or
the distance-estimation library.

## Current Windows execution

Install Nordic nRF Util, sdk-manager and NCS v3.4.0. The repository wrapper can
be called from an ordinary PowerShell; if `west` is not present it relaunches
itself through the pinned toolchain automatically.

Canonical production build:

```powershell
.\nRF54L15\ncs.ps1 build
```

Reproduce the MAX-45 proof:

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

The proof build checks the resolved `.config` for the SoftDevice Controller,
both BLE roles, one simultaneous connection, both CS roles, single CS
context/antenna and the existing 128-byte TWIS buffer.

## Historical verified evidence

On 2026-09-17, the MAX-45/MAX-49 gate built successfully with NCS v3.4.0:

- `MAX-45 KCONFIG_PROBE: PASS`;
- linked Channel Sounding proof image;
- FLASH 216244 B / 1524 KB (13.86%);
- RAM 43800 B / 256 KB (16.71%);
- wrapper exit `NCS BUILD = 0`.

Those numbers are build-resource evidence, not physical-ranging accuracy.

## Invariants

The proof and MAX-96 toolchain promotion do not change:

- one-image runtime MAIN/REMOTE model;
- 12-byte `SENSOR_REFRESH`;
- 102-byte REMOTE snapshot;
- ESP32 topology/sensor consumer;
- BMI160/BME280/orientation logic;
- geophone acquisition/TDOA;
- physical LED wiring;
- cross-node clock synchronization.

MAX-49 supplies the platform-neutral scheduler. MAX-51 is the separately
reviewed NCS RREQ/RRSP pair adapter. Multi-node execution authority/handoff is
also kept separate so `BT_MAX_CONN=1` does not create an implicit distributed
control race.
