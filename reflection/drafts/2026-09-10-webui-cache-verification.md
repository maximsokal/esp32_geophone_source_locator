# WebUI cache verification environment drift

- Evidence: the first host run after adding the optional embedded WebUI cache stopped at 27/28 because `esp_heap_caps.h` had no host stub. The first direct cache-on Ninja run reached the bootloader reconfigure and stopped because the child process did not have the ESP-IDF Ninja/toolchain PATH. The first full suite also caught one stale Unity contract call using the old three-argument waveform reducer.
- Root cause: an embedded-only include/configuration branch was not represented in host stubs, the alternate build was invoked without the wrapper environment, and a public test-app contract was not updated with the API change.
- Recurrence count: one confirmed episode for each issue; no generalized repository rule is proposed from this single task.
- Minimal proposal: when adding an optional ESP-IDF branch, add the narrow host stub and run both Kconfig variants with the documented wrapper/toolchain environment; when changing a public test API, search production, host, and `test_app` call sites before the aggregate suite.
- Resolution: added the `esp_heap_caps.h` host stub and error constants, reran host tests at 28/28, reran the cache-on build to completion, updated `test_app/main/test_app_contracts.c`, and finished `tools/run_suite.ps1 -Suite All` with `SUITE_RESULT: PASS`.
- Affected prior tasks: none known; this episode is local to the 2026-09-10 geophone performance/WebUI task.
