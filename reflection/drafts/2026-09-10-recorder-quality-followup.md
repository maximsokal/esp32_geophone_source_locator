# Candidate retrospective: follow-up quality work needed a concrete integration seam

- Evidence: the first actual `seismic_recorder_start` host integration build failed on missing `seismic_buffer` include and incomplete FreeRTOS/runtime stubs; the first Unity wrapper invocation also resolved `test_app/test_app/build-s3` because `BuildDir` was passed relative to the project path.
- Root cause: the new integration test was assembled incrementally without first mirroring the production component include/dependency surface, and the repository wrapper treats `BuildDir` as relative to `ProjectPath`.
- Recurrence count: 1 confirmed episode in this repository; this is not yet a generalized pattern.
- Minimal proposed change: for host integration tests, derive include directories and stubs from the production component before compiling; for `run_idf.ps1`, pass a build directory relative to the supplied project path and verify the resolved path before a long build.
- Affected task: recorder START regression and AD7606 rolling-quality follow-up on 2026-09-10.
- Validation for this correction: host C 28/28, production build, Unity build, Python suite, Web suite, and `git diff --check` all pass; no flash or monitor was run.
