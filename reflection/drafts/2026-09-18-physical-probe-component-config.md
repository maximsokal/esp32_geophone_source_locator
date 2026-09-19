# Candidate retrospective: physical-probe target component graph

- Evidence: the first target build after linking production AD7606, display,
  SD, sensor, SPI and Wi-Fi components stopped in
  `storage_manager_config_check.c` because the standalone target had inherited
  `CONFIG_FATFS_LFN_NONE=y`, `CONFIG_FATFS_ALLOC_PREFER_EXTRAM=y` and an
  insufficient `CONFIG_FATFS_MAX_LFN` configuration.
- Root cause: adding the repository component tree to a new ESP-IDF project
  expands the dependency graph and activates production configuration guards;
  the target-local defaults were initially limited to PSRAM and timer options.
- Recurrence count: 1 confirmed episode in this task.
- Minimal proposed rule: when a standalone ESP-IDF target links production
  storage or bus components, run a clean target-local configure before the
  first build and record every production component configuration guard that
  requires an explicit target default.
- Resolution: added target-local FATFS long-filename heap defaults, regenerated
  `sdkconfig`, and verified the clean build (`1155/1155`) plus the physical
  probe incremental rebuild (`9/9`).
- Affected prior work: the standalone-stress-sdkconfig draft is related, but
  this episode is specifically about production-component configuration guards,
  not the missing custom partition table.
