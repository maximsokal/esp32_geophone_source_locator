# nRF54L15 PlatformIO test surface

This directory is reserved for the future nRF54L15 PlatformIO test suites.
PlatformIO discovers suites in directories named `test_*` below this path.
The directory currently contains no executable suite; therefore
`..\pio.ps1 test` and the project-wide `Tests: All (no flash)` report an
explicit `SKIPPED` nRF test stage.

Add native/policy tests here only after the route is qualified against the
installed Seeed PlatformIO + Zephyr environment. Keep board-dependent checks
in explicit HIL work. Do not create central/remote test variants: every node
uses the same nRF firmware project and environment.
