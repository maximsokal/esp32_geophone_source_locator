# Build warning diagnosis retrospective

- Evidence: the ESP-IDF 5.5.4 build completed with exit code 0; the original gdbinit warning came from a missing `ESP_ROM_ELF_DIR`, and three LVGL warnings came from deprecated `lv_obj_remove_flag` calls. The Component Manager emitted `Missing kconfig option` and then completed its automatic retry. A trial that duplicated LVGL symbols in `main/Kconfig.projbuild` produced duplicate-symbol messages and was reverted.
- Root cause: actionable warnings were mixed with expected ESP-IDF configure-time messages; the first attempted Kconfig workaround duplicated ownership of managed-component symbols instead of respecting the LVGL Kconfig source.
- Recurrence count: one confirmed episode in this task; not enough evidence for a generalized rule.
- Minimal proposed rule/tool change: classify build output into compiler warnings, bootloader/environment warnings, and expected Component Manager rerun messages before editing Kconfig; never duplicate managed-component symbols to silence the rerun message.
- Affected prior work: LVGL 9.6 configuration migration and the ESP32-S3 IDE compatibility change in this repository.
