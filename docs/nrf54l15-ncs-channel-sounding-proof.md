# nRF54L15 NCS Channel Sounding build proof

This document defines the MAX-45 **build-only** proof path. It does not implement
pair scheduling, distance estimation, BLE discovery, GATT transport, or an ESP32
Channel Sounding protocol.

## Baseline

The proof is pinned to **nRF Connect SDK v3.4.0 LTS**. The v3.4.0 manifest uses
Nordic's Zephyr fork and already contains the upstream board target
`xiao_nrf54l15/nrf54l15/cpuapp`, so this proof does not need the separate Seeed
`platform-seeedboards` board root.

The NCS v3.4.0 XIAO board DTS also owns the board RF switch:

- `rfsw_ctl`: P2.05, `GPIO_ACTIVE_LOW`, `regulator-boot-on`;
- `rfsw_pwr`: P2.03, `GPIO_ACTIVE_HIGH`, `regulator-boot-on`.

That board definition selects/powers the default ceramic RF path before the
application enables Bluetooth. MAX-45 does not add application-owned RF-switch
GPIO toggling and does not add multi-antenna Channel Sounding.

The existing project `nRF54L15/zephyr/app.overlay` remains authoritative for the
project-specific dual-I2C topology:

- upstream D4/P1.10 SDA + D5/P1.11 SCL: `i2c22` switched from TWIM to TWIS;
- private D12/P0.04 SDA + D11/P0.03 SCL: `i2c30` remains TWIM/controller;
- the board-declared LSM6DS3TR-C is disabled because the diploma uses the plain
  XIAO plus an external BMI160.

## Why NCS is required

The project PlatformIO baseline packages ordinary Zephyr 4.2.1. The Zephyr host
has Channel Sounding APIs, but the nRF54L15 controller capability required by
`CONFIG_BT_CHANNEL_SOUNDING` is supplied by Nordic's SoftDevice Controller in
NCS. The NCS controller selects `BT_CTLR_CHANNEL_SOUNDING_SUPPORT` on hardware
with `HAS_HW_NRF_RADIO_CS` and supports initiator, reflector, or both roles.

MAX-45 therefore keeps the existing PlatformIO path unchanged while adding a
separate proof configuration. If this proof passes on the pinned Windows NCS
toolchain, the production nRF build should migrate to NCS/west in a later,
explicitly approved slice rather than maintaining divergent firmware sources.

## Proof configuration

`nRF54L15/ncs/channel_sounding.conf` is additive to the existing
`nRF54L15/zephyr/prj.conf`.

The common image is configured for both Bluetooth connection roles and both CS
roles:

- `CONFIG_BT_CENTRAL=y`
- `CONFIG_BT_PERIPHERAL=y`
- `CONFIG_BT_MAX_CONN=1`
- `CONFIG_BT_CTLR_SDC_PERIPHERAL_COUNT=1`
- `CONFIG_BT_CTLR_SDC_CS_ROLE_BOTH=y`
- `CONFIG_BT_CTLR_SDC_CS_COUNT=1`

`BT_MAX_CONN` is the host's maximum simultaneous connections. The reviewed first
ranging architecture uses only one active BLE/CS pair at a time, while a node
may act as central or peripheral on different sequential pair turns. NCS v3.4.0
contains an intentional SoftDevice Controller special case for
`BT_MAX_CONN <= 1`: when both central and peripheral roles are enabled it still
allocates one central role even with `CONFIG_BT_CTLR_SDC_PERIPHERAL_COUNT=1`.
Therefore `CONFIG_BT_MAX_CONN=1` preserves the required sequential dual-role
capability and avoids paying RAM for a second simultaneous host connection on
the 188 KiB XIAO target. `CS_COUNT=1` independently bounds Channel Sounding to
one procedure/context at a time.

The proof is also single-antenna and mode-3-disabled:

- one controller antenna path;
- one controller antenna;
- Channel Sounding test commands disabled;
- step mode 3 disabled;
- one result reassembly buffer.

No RAS profile or distance-estimation library is enabled by MAX-45.

## Windows execution

Install the official NCS v3.4.0 SDK + matching toolchain with Nordic's current
`sdk-manager` workflow, then open the toolchain terminal:

```powershell
nrfutil install sdk-manager
nrfutil sdk-manager install v3.4.0
nrfutil sdk-manager toolchain launch --ncs-version v3.4.0 --terminal
```

From that activated terminal, at the repository root:

```powershell
.\nRF54L15\ncs.ps1 build
```

The wrapper resolves the NCS west workspace in this order:

1. use `NRF_BASE` or `ZEPHYR_BASE` if the current environment explicitly exposes
   one of them and its parent is a valid west workspace;
2. try `west topdir` when the current directory already belongs to an NCS west
   workspace;
3. otherwise query Nordic's supported sdk-manager metadata:
   - `nrfutil sdk-manager config show` supplies the configured installation root;
   - `nrfutil sdk-manager list --all-fields` supplies the `SDK Directory` entry
     for exactly `v3.4.0`.

The third path is required because the Nordic toolchain environment documents the
toolchain variables it exports but does not guarantee `NRF_BASE` or
`ZEPHYR_BASE`, and this diploma repository normally lives outside the NCS west
workspace. If `install-dir` is `unset`, the wrapper uses Nordic's platform
default (`C:\ncs` on Windows, `$HOME/ncs` on Linux, `/opt/nordic/ncs` on macOS).
A custom configured install directory is therefore supported without hardcoding
`C:\ncs`.

After discovery, the wrapper still requires the selected workspace to report NCS
**exactly v3.4.0** through `nrf/VERSION` and requires the upstream XIAO board
directory to exist. A toolchain-only install is not enough: if sdk-manager does
not report the v3.4.0 SDK as installed, the proof fails before configuration.

The wrapper then builds the existing canonical `nRF54L15/zephyr` application for
`xiao_nrf54l15/nrf54l15/cpuapp` and adds only the MAX-45 CS fragment. After a
successful link it checks the resolved `.config` for the SoftDevice, dual BLE
roles, one maximum simultaneous connection, both CS roles, single CS
context/antenna, and the existing 128-byte TWIS buffer.

To build the unchanged baseline and the CS proof back-to-back and print ELF size
information for both:

```powershell
.\nRF54L15\ncs.ps1 compare
```

Other supported actions:

```powershell
.\nRF54L15\ncs.ps1 probe
.\nRF54L15\ncs.ps1 clean
```

`probe` never flashes hardware. MAX-45 is not complete until `build` (and, for
resource accounting, `compare`) is executed successfully in the pinned NCS
v3.4.0 Windows toolchain.

## Invariants

MAX-45 does **not** change:

- the one-image runtime MAIN/REMOTE bootstrap model;
- the 12-byte `SENSOR_REFRESH` request;
- the 102-byte REMOTE sensor snapshot;
- the ESP32 topology/sensor consumer;
- BMI160/BME280/orientation application logic;
- geophone acquisition or TDOA;
- physical LED wiring;
- cross-node clock synchronization.

The next Channel Sounding implementation slice, after this proof passes, should
first add a task-free five-node pair scheduler/data model. With five nodes there
are ten undirected pairs. Initial radio execution should serialize them with one
CS context per node and rotate initiator responsibility instead of making the
wired MAIN node a permanent Bluetooth master.
