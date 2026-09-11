# Web lifecycle mDNS owner and desired-state queue

- Evidence: audit of `4410c047` found mDNS initialization in the dedicated `web_lifecycle` task but mDNS stop in `wifi_manager_stop_radio()`, leaving unsynchronized `s_ready` state during fast STA-to-setup transitions. The same audit found a length-one `xQueueOverwrite()` carrying commands with distinct side effects.
- Root cause: network lifecycle ownership was split across task contexts, and the queue represented events while its overwrite semantics discarded intermediate events.
- Fix: moved all production mDNS stop calls into the web lifecycle worker, changed the queue payload to a desired network state, and added idempotent state-to-action plans for connected, setup AP, disconnected, and stopped states. Added executable host coverage for overwrite, AP transition, reconnect, shutdown, and action ordering.
- Recurrence count: this is the first confirmed occurrence of this exact split-owner/command-overwrite issue; do not generalize into a new global rule from one episode.
- Minimal rule proposal: when a queue has length one and uses overwrite, its payload must be a complete desired state with idempotent reconciliation, not a command/event whose side effects depend on delivery of every intermediate item.
- Verification: focused Python `20 passed`; Host C `33/33`; full `tools/run_suite.ps1 -Suite All` passed with HIL skipped; production image `0x1f9d30`, 51% app partition free. Hardware reconnect, mDNS advertisement, sys_evt HWM, CPU0 load, and TFT FPS remain unverified.
