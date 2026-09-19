# Candidate retrospective: updated boot log classified as a reset loop

- Evidence: the 2026-09-19 passive COM6 attachment contains one `rst:0x1 (POWERON)`, one SD `ESP_ERR_TIMEOUT`, boot-error UI setup, and repeated `RUNTIME_OBSERVABILITY` snapshots through 113 seconds. The source path `app_storage_boot_failure()` waits on the shutdown event with `portMAX_DELAY`; `main.c` starts nRF topology only after `app_startup_run()` returns.
- Root cause of the reporting correction: visual observation of the TFT was treated as proof that the ESP32 stopped, while the log showed a deliberate terminal storage-error state and live tasks. A checksum mismatch also means the flashed image was not the current local build.
- Recurrence count: one confirmed instance for this exact updated-log classification; prior SD timeout and USB-power episodes are related hardware evidence but not the same reporting mistake.
- Proposed minimal rule: for boot claims, classify reset/hang only from reset/BOD markers plus loss of periodic liveness; if a boot-error UI is followed by periodic task snapshots, report intentional wait. Require flashed app version/checksum alignment before attributing behavior to the current checkout.
- Affected prior tasks: ESP32 startup/SD/SPI2/XIAO power-path diagnosis and the COM4/COM6 evidence boundary. No AGENTS.md, tool, or skill change is proposed from one confirmed reporting episode.
- Verification follow-up: the focused Python boot-failure contract passed 4 tests, but the focused native `app_storage_boot_failure` manifest failed at compile because the new `freertos/FreeRTOS.h` include is not present in its host stub include path. This is a local verification gap in the newly appeared working-tree change, not evidence about the target board.

## Code-audit correction: false shared-CS diagnostic

- Evidence: `components/spi_bus/spi_bus.c` and `components/sd_card/sd_card_spi.c` configured the shared CS pins as `GPIO_MODE_OUTPUT`, while `components/sd_card/sd_card_mount.c` read them with `gpio_get_level()` in the mount-failure log. ESP-IDF disables the input path for output-only mode, so the resulting `TFT_CS=0` was a deterministic false readback.
- Root cause: the diagnostic contract and GPIO mode disagreed; the host fake always returned high and therefore could not expose this defect.
- Correction: configure both shared CS lines as input/output, drive both HIGH before SD discovery, and make the fake disable readback for output-only modes. Added assertions for CS mask/mode/order, real failure-log levels, second-CS-set failure, null card handle on nominal mount success, and SD retry success/failure timing.
- Recurrence count: one confirmed code defect in this audit; no generalized rule or agent-instruction change is proposed yet.
- Verification: native host manifest `51/51`, Python suite `457 passed, 1 skipped` (HIL disabled), ESP-IDF production build succeeded. Target flashing and physical HIL remain unclaimed.

## Flash blocker: download mode with no UART TX reply

- Evidence: the latest `ninja flash` completed image/partition checks and esptool reported `Download mode successfully detected`, then failed with `getting no sync reply: The serial TX path seems to be down`.
- Root cause classification: the PC-to-target boot/control path is responding enough to enter download mode, but the target-to-PC UART0 TX path is not returning the esptool sync frame. This is a physical serial topology, cable/USB-UART, power, or board-trace blocker; no source/build defect is indicated by this output.
- Project-specific next condition: isolate the ESP32 on COM4, disconnect the passive COM6 CH340 entirely during flashing (especially CH340 TXD and VCC), use BOOT+EN, then retry at 115200 if 460800 is unreliable. No hardware action was performed by the agent.
- Recurrence count: this is a new confirmed flash-transport episode; no generalized repository rule is proposed yet.

## Flash diagnosis correction: inspect host TX to ESP32 RX first

- Evidence: official esptool troubleshooting defines `Download mode successfully detected, but getting no sync reply: The serial TX path seems to be down` as the chip entering download mode and sending data to the host but not receiving the sync response from esptool.
- Correction: the primary line is host/CH343 TX to ESP32 UART0 RX/GPIO44, not ESP32 GPIO43 TX as previously stated. XIAO D6/D7, an accidentally connected CH340 TXD, a bridge to GPIO44/EN/GPIO0, or power/back-power contention are the first physical checks after assembly.
- Recurrence count: one confirmed interpretation correction in this flash episode; no generalized instruction change is proposed yet.

## Header-pin clarification: internal USB-UART path remains

- Evidence: the user confirms that the ESP32-S3 DevKitC GPIO44 header is not soldered externally; Espressif's DevKitC guide still identifies the onboard USB-to-UART port as the programming interface and labels the RX header as U0RXD/GPIO44.
- Correction: an unused GPIO44 header does not prove that the COM4 RX node is unconnected. Remove the direct external-wire hypothesis, but continue checking the onboard USB-UART trace, solder bridges, power stability, EN/BOOT interference, and XIAO UART/power connections.
- Recurrence count: one confirmed clarification; no generalized instruction change is proposed yet.

## XIAO removal does not remove the TCA/power-path hypothesis

- Evidence: the user reports the same COM4 flash failure with XIAO absent from the enclosure; repository search identifies TCA9548A as the only project multiplexer, connected to ESP32 GPIO9/GPIO10.
- Classification: TCA software selection/addressing cannot affect esptool because it runs after the ROM/bootloader flash phase. TCA hardware can still be an indirect fault through I2C pull-ups, 5 V level exposure, RESET wiring, back-power, downstream branches or 3.3 V rail loading.
- Minimal next test: keep ESP32 mechanically installed, disconnect TCA VCC, upstream SDA/SCL, RESET and every downstream branch, then flash COM4 at 115200; reconnect one branch at a time.
- Recurrence count: one confirmed narrowing episode; no generalized instruction change is proposed yet.

## XIAO and TCA physically removed: remaining COM4 transport fault

- Evidence: the user removed both XIAO and TCA, while the same `Download mode successfully detected, but getting no sync reply` persists; build and partition checks still pass.
- Classification: XIAO/TCA software and their direct physical branches are no longer sufficient causes. Keep only remaining enclosure/carrier wiring, CH340/external peripherals, EN/GPIO0/power, USB signal integrity, mechanical contact and onboard USB-UART path in the active fault set.
- Minimal next test: remove every external connection including the passive CH340 and peripherals, keep only the ESP32 installed plus COM4, retry `chip_id`/flash at 115200, then compare with the already-successful out-of-enclosure position.
- Recurrence count: one confirmed isolation result; no generalized instruction change is proposed yet.

## AD7606 A/B isolation result

- Evidence: with AD7606 removed, ESP32 still installed in the enclosure flashes; XIAO and TCA can then be inserted and flashing still succeeds; putting AD7606 back restores the COM4 no-sync failure.
- Classification: the active hardware fault is isolated to AD7606, its power/ground, VDRIVE/logic levels, GPIO branch or module wiring. This is stronger evidence than the earlier XIAO/TCA correlation.
- Minimal next test: disconnect all AD7606 signal and supply wires, verify 5 V analog and 3.3 V digital VDRIVE, verify GPIO41 pull-up and pin map, then reconnect only after measuring rail stability and checking for shorts/back-power.
- Recurrence count: one confirmed A/B isolation episode; no generalized instruction change is proposed yet.

## AD7606 rail measurement narrows the fault

- Evidence: with AD7606 attached, 5V remains about 4.935V/4.948V, but ESP32 3V3 is only 2.8V and AD7606 VIO/VDRIVE is 2.88V; GPIO39/47/2 readings are near the sagged 3V3 domain. This is consistent with a 3.3V load or regulator/level-wiring fault and explains a ROM download-mode/no-sync failure.
- Correction: the reported 0 Ohm unpowered readings on AVCC and GPIO39 are not accepted as hard shorts yet because the corresponding powered nodes are live; repeat after capacitor discharge, reverse probes and compare disconnected/connected states.
- Minimal next test: do not flash with AD7606 attached; compare ESP32 3V3 and AD VIO with the module disconnected, then measure VIO origin and verify it is a 3.3V digital supply rather than a conflicting module regulator/output.
- Recurrence count: one confirmed rail-measurement narrowing episode; no generalized instruction change is proposed yet.
