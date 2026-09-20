# Retrospective candidate: capacitor test was confounded by software shutdown

- Evidence: the post-capacitor log never reaches Wi-Fi/PHY; SD initialization fails first, then GPIO21 is latched for exactly the configured 150 ms and the app sends the Pololu OFF pulse before `E BOD`.
- Root cause of the misleading conclusion: the run was treated as a rail/Wi-Fi experiment even though the firmware's active-high GPIO21 input has internal pulls disabled and the captured sequence is an intentional shutdown path.
- Recurrence count: this is the first confirmed instance in this task of the capacitor A/B being judged from a run that never reached PHY; prior brownout logs are separate evidence.
- Minimal rule proposal: classify a capacitor/power experiment as valid only when the log reaches the targeted stage (for Wi-Fi, `STA PHY bring-up begin`) and contains no SD-failure shutdown or GPIO21 shutdown sequence; report the exact stage otherwise.
- Affected area: ESP32 startup/power diagnosis and hardware-test log interpretation. No production code was changed.
