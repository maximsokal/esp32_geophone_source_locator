# Candidate retrospective: AD7606 boot GPIO assumption

- Evidence: the latest COM6 log shows AD7606 GPIO initialization before the SD probe; source configures CS as an output and drives it high, and the board contract adds a 10 kOhm pull-up to 3.3 V. The user measured VIO backfeed when the AD7606 5 V rail was absent.
- Root cause: the new early `ad7606_driver_safe_gpio_init()` path assumes the ADC is powered from board boot and calls a state “safe” without modeling an unpowered peripheral. The same log's apparent stall is a separate intentional boot-error wait followed by GPIO21 long-press shutdown.
- Recurrence count: one confirmed episode in this task; do not generalize into a workflow rule until a second independent episode is confirmed.
- Proposed minimal rule: every “safe GPIO” contract for a peripheral with a separate supply must specify the unpowered state and test high-Z/backfeed behavior, not only logical idle levels. Add a host regression for unpowered AD7606 initialization and a physical power-good gate before production validation.
- Affected prior task: the earlier AD7606/COM4 diagnosis treated the failure as hardware-only until the new source audit connected the A/B result with the early GPIO initialization.

## Follow-up evidence

- The exact `f10667b4` image repeatedly resets with `Saved PC: rtc_brownout_isr_handler`; several resets occur during the 250 ms pre-mount delay, before SD response. The final cycle alone reaches the boot-error button shutdown and its deliberate OFF pulse.
- This separates the primary brownout loop from the secondary GPIO21 shutdown path. The delay change did not solve the rail failure; it only moved the observation point. The software GPIO-power assumption remains actionable, but the repeated reset still requires physical rail/backfeed validation.

## A/B correction

- The follow-up image removed the early AD7606 GPIO initialization and no longer links it from `main`; the log has no AD7606 GPIO configuration line but still brownouts during SD probing. This disproves necessity of that call for the reset loop. Keep the finding as an unsafe code defect, but classify the primary loop as unresolved physical rail/backfeed until the AD7606 wiring is isolated or measured under load.

## Regression candidate found

- The same image still contains the recent SD/SPI refactor: `spi_bus` owns both CS idle levels, while `sd_card_internal_prepare_shared_spi()` reconfigures both CS pins and drives SD_CS high again before every probe. The new log's extra `SD CS forced HIGH` is the last new application action before SD traffic and brownout. This duplicate GPIO ownership is the leading code candidate for the user's reported regression; validate by reverting only this change and the retry loop, without disabling brownout.

## Stale-image and recovery-log correction

- Evidence: the latest pasted log reports `object_recovery_run=30500 ms` before Wi-Fi and performs `unlink(object.json.tmp)` for every valid object. Current `main/app_startup.c` only calls `object_recovery_run_active()` before services and queues full recovery after `startup_total`; current metadata recovery returns immediately for valid primary metadata. The pasted file then appends a separate `427a8c4d` boot with an explicit flashed/built checksum mismatch.
- Root cause of the long SD load: the observed image is older than the current recovery/startup fixes, not proof that the current checkout still blocks on the full archive. The reset after `phy_init` remains unproven in this paste because its `rst:`/BOD marker is missing; prior matching-image evidence reported brownout at this Wi-Fi radio-start boundary.
- Recurrence count: one additional confirmed stale-image episode; retain as a task-specific correction until a second independent clean capture confirms it.
- Proposed minimal rule: every hardware log used for source diagnosis must include one clean boot header with app version, compile time, checksum status, and the first reset marker; reject concatenated monitor buffers as causal evidence.

## Current-image brownout confirmation

- Evidence: a new clean capture identifies `f10667b4-dirty`, mounts SD successfully, completes active recovery in about 0.55 s, reaches `interactive_ready`, then logs `WEB_SERVER: STA connecting`, `phy_init`, `E BOD`, `rtc_brownout_isr_handler`, and `rst:0x3`; later cycles also show `rst:0xf (BROWNOUT_RST)`. The same sequence repeats on another USB source.
- Root cause: the current source path is no longer blocked by archive recovery or stale firmware. `esp_wifi_start()` is the last application operation before the brownout, so Wi-Fi radio startup is the trigger that exposes the 3.3 V/power transient; this is not an application panic/backtrace. The earlier measured 2.8 V ESP32 3V3 with AD7606 attached remains consistent with an external rail/load fault.
- Recurrence count: two independent current-image brownout captures at the Wi-Fi boundary; this is now a confirmed pattern, while the exact electrical element causing the 3.3 V dip remains unisolated.
- Proposed minimal rule: keep BOD enabled and treat `phy_init -> E BOD -> brownout ISR` as rail evidence. Any software A/B may only delay/bypass Wi-Fi to localize the trigger; it must not disable BOD or be reported as fixing the power fault.

## Deeper Wi-Fi boundary audit

- Evidence: the clean `f10667b4-dirty` log reaches `WEB_SERVER: STA connecting`, then `phy_init`, then `E BOD` with `Saved PC ... rtc_brownout_isr_handler`; it never reaches the `after-wifi` snapshot or the application's `STA modem power save disabled` log. Source confirms `esp_wifi_start()` is at `components/wifi_manager/wifi_manager_sta.c:42`, while `WIFI_PS_NONE` is called only afterward at lines 56-59. The active build has `CONFIG_ESP_PHY_ENABLE_USB=y`; IDF enables the USB PHY in `phy_init.c:916-918`. `CONFIG_ESP_PHY_REDUCE_TX_POWER=y` is only applied after `esp_reset_reason()==ESP_RST_BROWNOUT` at lines 905-906. The focused Python contract set passed 20 tests.
- Root cause classification: the code's synchronous Wi-Fi start with all other loads active is the trigger point, and `WIFI_PS_NONE` is an additional later power regression. The immediate reset is still a 3.3 V rail collapse exposed during PHY initialization; current evidence does not justify blaming XIAO/TCA runtime code, AD7606 sampling, or a C panic. Disabling USB PHY is a targeted software A/B for a UART-console build, not proof of a hardware fix.
- Recurrence count: two independent clean current-image captures at the same PHY boundary; the exact physical rail element remains unresolved.
- Proposed minimal rule: for power-sensitive ESP32 startup, test and log the pre/post `esp_wifi_start()` boundary, keep modem power-save enabled by default, and treat PHY USB-keepalive and TX-power recovery options as explicit power-budget decisions. Never turn off brownout detection to make the boot appear stable.

## I2C diagnostic reclassification

- Evidence: the new boot shows `i2c.master: this port has not been initialized`, immediately followed by `i2c0 bus inited`, `I2C Bus V2 Config Succeed`, and later `INA3221 online` with valid samples. The pinned managed component first probes `i2c_master_get_bus_handle()` to support an externally initialized bus; IDF logs the expected absent-handle result at error level before the component installs its own bus.
- Root cause classification: this message is a misleading dependency log, not the source of the reset loop or proof of an application init-order race. It should be cleaned up only by updating/patching the dependency, not by pre-initializing a second I2C owner in `sensor_bus`.
- Recurrence count: one confirmed occurrence in the current image; classify as benign dependency noise unless a future boot lacks the following successful bus-init/INA-online lines.

## Diagnostic-load regression found

- Evidence: the current dirty worktree added `ina3221_monitor_request_fast_sampling(5000)` immediately before `wifi_manager_start()`. Its implementation changes the monitor from 1000 ms to 20 ms, reads all six INA3221 measurement registers each cycle, and logs every sample. The latest boot-loop log contains only second-stage bootloader output (`rst:0xf`) and no application entry; the previous app-level logs showed the brownout at `phy_init`. The diagnostic call therefore changed the exact workload under test and could worsen the PHY boundary failure.
- Root cause: automatic high-rate INA telemetry was an invalid production-startup experiment, not a passive measurement. It was removed from `app_startup.c`; the helper remains explicit-only. A Python contract prevents reintroducing it before Wi-Fi.
- Recurrence count: one confirmed source-level regression; no claim that it explains the original brownout until the corrected image is run.
- Verification: focused Python contracts 21 passed; native host suite 51/51; ESP-IDF build passed. No flash or HIL proof yet.

## Deeper startup-load correction

- Evidence: git history shows commit `17258bce` moved `web_server_start()` and runtime `ui_init()` ahead of `wifi_manager_start()`, commit `6bc97cb3` moved keypad ingress ahead of network, and commit `427a8c4d` changed the LVGL refresh timer to 42 ms. The current log reaches the `phy_init` brownout only after the UI and Web tasks are active, so their work overlaps the synchronous radio bring-up. The same audit found INA3221 startup before Wi-Fi and a later `WIFI_PS_NONE` policy.
- Root cause classification: these are confirmed software power-load regressions and have now been removed from the boot path; they are strong trigger candidates, but target/HIL evidence is still required to prove whether they explain the rail collapse.
- Recurrence count: two independent current-image PHY-boundary brownouts plus one source-level UI-order regression; do not promote this to a general workflow rule yet.
- Verification: added pre/post `esp_wifi_start()` logs and static ordering contracts; focused Python contracts 30 passed and ESP-IDF build passed. No flash or HIL proof yet.

## Web task boundary correction

- Evidence: `web_server_start()` creates the status-cache and lifecycle tasks before registering network handlers. The known-good `844437f9` startup started Wi-Fi first; the failing branch started Web/UI/keypad before Wi-Fi. The current source now starts Wi-Fi, then Web, then dynamic UI/keypad/INA.
- Root cause classification: this removes another confirmed pre-PHY software load regression, but the physical rail cause remains unproven until the corrected image runs on the target.
- Recurrence count: two independent current-image PHY-boundary brownouts and two source-level startup-order regressions; keep this as project-specific evidence, not a generalized rule.
- Verification: focused Python 28 passed, full Python 463 passed with one explicit HIL skip, native host 51/51 from the unchanged host path, and ESP-IDF target build passed. No flash or HIL proof.
