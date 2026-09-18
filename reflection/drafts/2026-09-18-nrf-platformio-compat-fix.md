# nRF PlatformIO compatibility configuration: candidate retrospective

- Evidence: before the fix, `nRF54L15\pio.ps1 -Action build` failed in Zephyr `4.2.1` Kconfig with undefined or dependency-unsatisfied SoftDevice, RAS, Channel Sounding, and CS estimator symbols from the production `zephyr\prj.conf`.
- Root cause: one Kconfig file was incorrectly consumed by two incompatible toolchains. The production file is for pinned NCS `v3.4.0` / Zephyr `4.4.0`; PlatformIO is a historical Zephyr `4.2.1` compatibility path.
- Correction: CMake selects `zephyr\prj.platformio.conf` only when `PIO_PACKAGES_DIR` is defined. NCS production and the Channel Sounding proof continue to consume `prj.conf`; no production symbols were weakened and no blanket warning suppression was added.
- Verification: the old failure was reproduced; the corrected PlatformIO build succeeded and its resolved `CONF_FILE` points to `prj.platformio.conf`; nRF host coverage passed `18/18`; targeted Python contracts passed `20`. The `test_sw_*` lane remains an intentional `SKIPPED` because no reviewed native embedded suite exists.
- Recurrence count: one confirmed episode. Proposed minimal rule: when toolchain-specific Kconfig is promoted, keep an explicit compatibility fragment and test the resolved configuration/build result.
- Scope: proposal only; do not modify shared agent rules without separate approval.
