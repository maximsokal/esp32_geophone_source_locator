# Web lifecycle quiesce ordering follow-up

- Evidence: the audit of `4410c047` found mDNS initialization in the dedicated
  `web_lifecycle` task but mDNS stop still in `wifi_manager_stop_radio()`;
  mode changes could therefore call `esp_wifi_stop()` before the worker had
  stopped mDNS. The same audit identified a one-slot command queue whose
  overwrite behavior needed an executable state-reconciliation regression.
- Root cause: ownership was moved for startup but the mode-transition boundary
  had no synchronous acknowledgement from the lifecycle worker. The queue had
  desired-state semantics in production, but the test did not previously model
  an in-progress worker plan and an overwrite.
- Fix: added the `QUIESCED` state with mDNS-stop and HTTPS-stop actions, a
  synchronous prepare callback/ack before every Wi-Fi radio stop, and explicit
  rejection of a failed quiesce. Added host tests for worker-plan overwrite
  reconciliation and the service-to-radio ordering contract.
- Recurrence count: one confirmed episode for this exact split-owner/order
  defect; do not generalize the rule yet without another independent case.
- Proposed minimal rule: when a service owns a resource across a hardware mode
  transition, expose one synchronous quiesce barrier owned by that service;
  test both the barrier ordering and last-desired-state reconciliation.
- Verification: Host C `34/34`; Python `56 passed + 1 skipped`; Web Console
  `38/38`; Web XR `582/582`; typecheck, Unity build, and production build all
  passed. Production image `0x1fa1c0` with `51%` app-partition free. HIL,
  flashing, serial monitor, and physical reconnect/mode-switch behavior were
  not run.
