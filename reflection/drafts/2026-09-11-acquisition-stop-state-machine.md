# Acquisition stop state-machine bypass

- Evidence: audit of main 6833d21 found web_control_acquisition_stop() directly calling seismic_recorder_stop() and seismic_recorder_wait_for_stop() while measurement_service could still report RECORDING. The same audit found web_control_measurement_start() rejecting cache-unready status before submitting the canonical START.
- Root cause: acquisition lifecycle and recorder lifecycle had remained coupled in an older compatibility endpoint after measurement_service became the owner of recorder transitions. The Web START path also retained an obsolete status-cache readiness precondition.
- Fix: acquisition_stop now fails closed from a measurement snapshot while the service is busy or in STARTING/RECORDING/STOPPING, and it only calls seismic_acquisition_stop() after the measurement service is idle/review/error. Web measurement_start submits without a cache_ready check. WebUI ownership and disabled controls follow measurement.state.
- Regression evidence: the host measurement-service test exercises queued START -> RECORDING -> canonical STOPPING -> REVIEW and asserts the acquisition-stop policy at every state; Python source contracts and Web Console tests cover the API/UI wiring.
- Recurrence count: one confirmed episode in this task; no generalized skill or AGENTS rule is proposed.
- Verification: focused and full suites passed, including Host C 29/29 and ESP Unity/production builds. No flash, serial monitor, TARGET/HIL, or physical verification was performed.
