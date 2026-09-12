# Candidate retrospective: ESP-IDF build-directory boundary

- Evidence: a direct Ninja/IDF invocation against the historical `build` directory changed its cached generator, so the normal wrapper could no longer reuse that directory. A clean isolated `build-refactor` directory built successfully with the same ESP-IDF 5.5.4 toolchain; `tools/run_suite.ps1` now accepts `-ProductionBuildDir` while retaining the historical default.
- Root cause: the build directory was treated as disposable even though its CMake generator/cache belonged to the existing checkout workflow.
- Recurrence count: 1 confirmed episode.
- Proposed minimal rule: use `tools/run_idf.ps1` and a new, explicitly named build directory for migration builds when the existing cache/generator is not known to match; never infer a source failure from a foreign generator cache.
- Affected task: Phase A post-refactor production compile on 2026-09-12.
