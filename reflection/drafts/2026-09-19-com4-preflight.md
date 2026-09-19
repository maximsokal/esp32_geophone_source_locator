# Candidate retrospective: require live COM4 before ESP32 stress HIL

- Evidence: the corrected stress runner was invoked with the verified image and both COM4/COM6. The preflight stopped before flashing because only COM6 was live; COM4 was listed by PnP as `Unknown` and `mode COM4` failed.
- Root cause: the programming-port connection was not live at the moment of the physical HIL attempt; the runner correctly refused to flash or capture a partial run.
- Recurrence count: 1 confirmed episode.
- Proposed minimal rule: keep the serial-port preflight immediately before any flash, and treat a PnP entry as insufficient until the port opens as a live serial device.
- Affected task: ESP32 stress HIL execution.
