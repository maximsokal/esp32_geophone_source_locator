# PocketBeagle 2 Rev A0 — PRU-SS Build Environment (AM6232)

> **Phase 1 Status:** Build environment preparation & documentation only. PRU firmware implementation is gated by **Phase 4 (Issue #6)** after hardware safety & KiCad review (**Phase 3 / Issue #48**).

## Target Architecture
- **SoC:** TI AM6232 (PocketBeagle 2 **Rev A0** — NOT A1/AM6254, NOT PRU-ICSSG).
- **Subsystem:** `PRU-SS` (dual 32-bit RISC PRU cores: `PRU0`, `PRU1`).
- **Intended Role Assignment:**
  - `PRU1`: Deterministic AD7606 4-channel acquisition (`CONVST`, `RESET`, `CS`, `SCLK`, `BUSY`, `DOUTA`, optional `DOUTB`/`FRSTDATA`).
  - `PRU0`: Optional auxiliary block transfer / ring drain if profiling proves beneficial.

## Required Toolchain (for Phase 4)
1. **TI PRU C/C++ Compiler (`clpru` / TI PRU CGT):**
   - Official TI download / Processor SDK AM62x (`Getting_Started_with_PSSP.html`).
   - Alternative Debian package (`gcc-pru`) may be evaluated, but TI PSSP linker/resource-table headers target `clpru`.
2. **TI PRU Software Support Package (PSSP):**
   - Source: `https://git.ti.com/cgit/pru-software-support-package/pru-software-support-package/`
   - Provides AM62x device headers, `rsc_table` templates, and RPMsg control libraries.
3. **Linux Loading Mechanism:**
   - Loaded via Linux `remoteproc` (`/sys/class/remoteproc/remoteproc*`).
   - Always discover PRU nodes by `name` attribute, **never** hardcode `remoteproc0` or `remoteproc1`.
