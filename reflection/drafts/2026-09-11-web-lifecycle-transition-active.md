# Web lifecycle transition gate retrospective

- Evidence: the audit of `e2de272` found that the quiesce ACK cleared the
  lifecycle gate before `esp_wifi_stop()` ran. A network event could then
  restart mDNS/HTTPS while the radio transition was still in progress.
- Root cause: one `quiesce_pending` flag represented both worker quiescence
  and completion of the whole radio-stop transaction.
- Fix: split the phases. `transition_active` remains set from prepare through
  the post-`esp_wifi_stop()` completion callback; network states received in
  that interval are coalesced as the latest deferred state. Callback
  unregister waits for the complete lifecycle to finish.
- Recurrence count: one confirmed occurrence of this exact race. Keep the
  rule scoped to this web/Wi-Fi lifecycle until another independent occurrence
  supports a broader process change.
- Verification: focused Host C 34/34 and Python policy 20/20; full
  `tools/run_suite.ps1 -Suite All` passed, including Unity and production ESP
  builds. HIL/flash/serial/physical verification was not run.
