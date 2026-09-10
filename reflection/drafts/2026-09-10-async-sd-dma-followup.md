# Candidate retrospective: async measurement completion and SDSPI DMA staging

- Date: 2026-09-10
- Evidence: objective audit of `cab0505179aa88804e5744ab68845799e448a565`; ESP-IDF 5.5.4 `sdmmc_write_sectors()` and `sdspi_host.c` source inspection; focused WebUI/host/firmware build checks.
- Root causes: `measurement_start`/`measurement_stop` return after queue submission while `DeviceConsoleApp` immediately read the SD-backed session list; the 512-frame recorder copy was allocated in PSRAM even though the SDSPI path copies external-RAM sources into a temporary DMA block per sector.
- Correction: expose measurement-service state in `/api/status`; poll until `RECORDING`, `REVIEW`, or terminal `ERROR` before refreshing sessions; retain PSRAM as backlog storage but copy bounded 256-frame chunks through a `DMA_ATTR` internal staging buffer inside one storage-operation window.
- Verification: WebUI typecheck PASS; console tests 33/33; focused Python contracts 7 passed; host C 28/28; ESP-IDF production build PASS. HIL/flash/monitor/physical proof intentionally absent.
- Recurrence count: 1 confirmed episode for this specific async session-refresh/DMA-source combination; do not generalize into a repository-wide rule without another confirmed episode.
- Proposed minimal rule: every asynchronous storage mutation must expose a terminal status that the UI waits on before reading its derived SD model; every SDSPI write path must make the DMA-capability of its `fwrite` source explicit and testable.
- Affected prior task: the `cab0505` recorder throughput/WebUI correction.
