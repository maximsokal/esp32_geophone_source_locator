# nRF54L15 PlatformIO test surface

This directory is reserved for future nRF54L15 test suites.

The automatic project-wide no-flash path is deliberately restricted to suites
named:

```text
test_sw_*
```

Only suites that have been explicitly qualified as hardware-free software/native
tests may use that prefix. `..\pio.ps1 test` and `Tests: All (no flash)` ignore
physical target/HIL suites.

PlatformIO's `--without-uploading` flag prevents firmware upload but does not by
itself guarantee that an embedded suite is hardware-free or that it will not
need a serial test port. For that reason board-dependent tests must use a
separate explicit future HIL runner/naming convention and must never be added
to `test_sw_*` merely to make them run from the master suite.

There is currently no executable `test_sw_*` suite, so the automatic nRF test
stage reports explicit `SKIPPED`.

Do not create central/remote test firmware variants: all physical nodes use the
same production nRF firmware. A host/native-only test environment is permitted
if needed for software tests and does not count as a second production
firmware variant.
