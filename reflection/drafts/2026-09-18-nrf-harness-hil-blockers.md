# Reflection draft: nRF behavioral harness, native runner, and HIL blockers

## Evidence

- `nrf_topology_service_get_leveling_summary()` had only source-contract coverage. The target Unity app now builds the real ESP-IDF service with a test-only state injection seam and calls the public API; the target build completed `925/925`.
- The first target harness build stopped in the unrelated storage configuration seam because the minimal test app did not provide FAT LFN settings. `tests/target/test_app/sdkconfig.defaults` supplied the required target defaults; the unchanged build then passed.
- `nRF54L15/test` had no `test_sw_*` directory. A native `test_sw_upstream_protocol` suite now exists and covers the 112-byte codec plus rejection of the old 102-byte length.
- The local PlatformIO native runner cannot execute because no `gcc` executable is available. The wrapper now reports `BLOCKED` with exit code 2 instead of falsely implying a pass or silently retaining the old empty-suite `SKIPPED` result.
- The physical 112-byte XIAO/TCA/Channel Sounding re-run could not start: only the XIAO appeared as COM5; the required ESP32/TCA/peer setup was absent. No flash or physical HIL was performed.

## Root cause

The test inventory and evidence boundaries were incomplete: source contracts were mistaken for executable service behavior, the native suite directory was empty, and the physical gate had not been rebased after the snapshot grew from 102 to 112 bytes. The local environment also lacks the host compiler required by PlatformIO's native builder.

## Recurrence count

1 confirmed occurrence in this task. Do not generalize into a repository-wide rule until a second independent occurrence is observed.

## Minimal proposal

Keep a target Unity harness for public ESP-IDF service behavior, require at least one executable `test_sw_*` suite before calling the native surface complete, and make toolchain/board absence an explicit `BLOCKED` result. Never promote a target build, replay, or source-contract pass to physical HIL evidence.

## Affected task

The work maps to the existing nRF coordination/protocol and physical-validation tasks (MAX-114 and MAX-38); no duplicate Linear task is needed for this correction.
