# Candidate retrospective: Phase B build boundary

- Evidence: focused Phase B contracts passed `10/10`; host C passed `36/36`; ESP-IDF 5.5.4 Unity target passed. Production configure discovered `components/app_events`, then stopped at `components/power_control/CMakeLists.txt` because `power_button_adapter.c` was missing.
- Root cause: another worker was concurrently migrating the input modules and had already removed power-button sources while leaving a CMake reference; the task explicitly prohibited changing `power_control` and keypad.
- Recurrence count: 1 confirmed episode in this task; insufficient evidence for a generalized process rule.
- Minimal proposal: when concurrent workers own adjacent components, run the narrow target/component checks first and record exact foreign configure blockers instead of repairing outside the requested slice.
- Affected prior tasks: none established from this episode.
