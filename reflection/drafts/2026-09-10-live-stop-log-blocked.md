# Live STOP-log validation remains blocked

- Evidence: HEAD is `d7548e7fe33d4a89621d7d533fc350b34dc118fe`; `SerialPort.GetPortNames()` reports only `COM6`. `COM4` is present only as an `Unknown` PnP entry, while COM6 is owned by an existing Espressif IDE `idf_monitor` process.
- Commands: `tools/check_serial_ports.ps1 -Ports COM4,COM6` exited 1; `tools/run_pipeline_hil.ps1 -SkipFlash -BaseUrl http://192.168.4.1 -TimeoutSeconds 5 -PollIntervalMs 250` returned `PIPELINE_HIL_RESULT: BLOCKED` before monitor/flash.
- Root cause: the requested real START→30–60 s→STOP observation requires the board's programming COM4 and a usable passive log/API path; current external state provides neither a live COM4 nor a safely capturable new log.
- Recurrence count: 2 confirmed no-live-port episodes in the project history (2026-09-08 and this continuation).
- Minimal rule: treat live-port preflight as the hard gate; never flash through COM6 or terminate an existing monitor to bypass missing COM4. Resume only after COM4 is live and the dedicated HIL object/API details or a captured STOP log are available.
- Affected work: current `d7548e7` AD7606 diagnostics handoff; prior ESP32 HIL verification work in `memory/2026-09-08.md`.
