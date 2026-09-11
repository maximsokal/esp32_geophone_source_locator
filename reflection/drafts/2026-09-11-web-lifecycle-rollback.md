# Web lifecycle rollback retrospective

- Evidence: the audit of `5b96d4b` showed that a successful web quiesce
  followed by a failed `esp_wifi_stop()` left the radio marked running while
  mDNS and HTTPS stayed stopped.
- Root cause: the completion path only released `transition_active` and
  reconciled a deferred event; it had no snapshot of the last stable desired
  network state for the no-event failure case.
- Fix: track the last accepted non-quiesced stable state, snapshot it at
  prepare, and on prepare/radio-stop failure reconcile the newest deferred
  state or the rollback snapshot. Normalize `ESP_ERR_WIFI_NOT_STARTED` to
  `ESP_OK` before invoking the completion callback.
- Recurrence count: one confirmed occurrence of this exact failure path. Keep
  the rule scoped to the web/Wi-Fi lifecycle until another independent case is
  confirmed.
- Verification: Host C `34/34`, focused Python `20/20`, and full `Suite All`
  passed, including Unity and production ESP builds. HIL/flash/serial/
  physical verification was not run.
