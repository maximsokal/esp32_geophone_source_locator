# Candidate retrospective: Phase H/I DSP verification

- Evidence: the focused host DSP executable passed; the ESP-IDF target component
  build passed and compiled `seismic_dsp.c` plus official `espressif/esp-dsp`
  1.8.2. The full production wrapper stopped at a missing
  `components/keypad/keypad_adc.c` after unrelated concurrent tree changes.
- Root cause: production verification used the shared dirty checkout, where
  another workstream was removing or replacing input components. Component
  Manager also regenerated dependency state during configuration, so the lock
  file had to be restored and hash-checked afterward.
- Confirmed recurrence count for this task: 1. This is evidence for a local
  verification hazard, not yet a generalized rule.
- Minimal proposed improvement: record a pre-build status/hash snapshot and
  use an isolated or lock-preserving production configure when concurrent
  workstreams are active; never classify a full production build as a DSP
  failure when the compiler stops in an unrelated missing source file.
- Affected prior tasks: none established by this task.
