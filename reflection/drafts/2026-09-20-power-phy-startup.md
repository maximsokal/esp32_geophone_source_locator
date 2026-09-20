# Candidate retrospective: PHY startup power evidence

- Evidence: the product page advertises 5 V/3 A/15 W but gives no load-step, ripple, or empty-battery operating specification; the user's repeated log shows `rst:0x1 (POWERON)` immediately after `phy_init`, while the application reaches that point consistently.
- Root cause: the diagnosis must separate average current rating from transient behavior and must inspect the complete startup path. The application still starts boot LVGL and SD/storage work before Wi-Fi, so a power-path cutoff can be software-amplified. Missing RF calibration in NVS can also force ESP-IDF from configured partial calibration to full calibration.
- Correction: do not describe the module as a "small-current" supply solely from an INA3221 average sample. First eliminate simultaneous USB and 5 V-pin power, then obtain one successful PHY calibration boot on a stable source, and only then compare startup-order variants.
- Recurrence count: 1 confirmed episode; not enough evidence for a generalized workflow rule yet.
- Proposed minimal rule: for embedded brownout/reset diagnosis, record reset reason, distinguish average telemetry from load-step behavior, and audit all pre-radio tasks plus RF-calibration state before assigning blame to the radio driver.
