# Candidate retrospective: Phase E build touched a forbidden lock file

- Evidence: `cmake --build build --parallel 2` triggered CMake reconfigure and Component Manager dependency solving; its output explicitly said `Updating lock file at E:\Beetroot Academy\Embedded\esp32_geophone_source_locator\dependencies.lock`. The resulting lock added unrelated `button`, `esp_insights`, `led_indicator`, and RainMaker dependencies. The build then failed on the pre-existing missing `components/power_control/power_button_adapter.c`.
- Root cause: Ninja's automatic CMake regeneration ran Component Manager because the dirty checkout's manifest hash did not match the lock. The command was treated as a build-only check, but the project build system performs a mutating dependency solve during regeneration.
- Recurrence count: 1 confirmed episode in this repository task; this is evidence for a local warning, not a generalized rule yet.
- Proposed minimal rule: before ESP-IDF build in a dirty checkout, record and isolate `dependencies.lock`; prefer a preconfigured build or a separate lock path, and never leave Component Manager output in a user-owned lock when the task forbids that file.
- Affected prior tasks: none confirmed from this checkout; prior ESP-IDF runs may also have regenerated managed dependencies, but no second confirmed forbidden-lock episode is recorded here.
