# Wi-Fi target-start failure retrospective

- Evidence: review of `93cbcb3105fff8a55a9e20d3e7e3757610ddd0c6` identified that `wifi_manager_enter_setup_mode()` and `wifi_manager_connect_saved()` destroyed the old radio successfully, then returned a target-start error without finalizing manager state or reconciling Web lifecycle resources.
- Root cause: the mode functions treated radio stop and target start as separate success paths; `s_running` and `s_setup_mode` were committed only after target success, with no failure transaction boundary.
- Minimal fix: after target failure, stop a partially started target, clear reconnect/setup state, commit the only honest fallback (`running=false`, `setup_mode=false`), and notify Web lifecycle. Add a callback drain on teardown and an IP check for successful STA notification so stale completion cannot overwrite a newer GOT_IP state.
- Recurrence count: one confirmed instance of this exact failure class; keep the rule scoped to mode-transition target-start failures until another independent occurrence is confirmed.
- Verification: Host C `34/34`, Python `56 passed + 1 skipped`, Web Console `38/38`, Web XR `582/582`, typecheck, Unity and production builds passed in `tools/run_suite.ps1 -Suite All`; HIL/flash/serial/physical verification was not performed.
