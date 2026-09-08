# Candidate retrospective: target-only runtime API entered a host contract

- Evidence: the first host run after switching service tasks to ESP-IDF
  `xTaskCreatePinnedToCoreWithCaps()` failed because the measurement-service
  production source is compiled into the native contract test and its host
  stubs did not define `esp_heap_caps.h` or `freertos/idf_additions.h`.
- Root cause: the target-only API boundary was changed without updating the
  explicit host-test seam at the same time.
- Recurrence count: 1 confirmed episode; this is not yet evidence for a
  generalized rule change.
- Minimal proposal: whenever a production component is included directly by a
  host contract test, add/update its target API stubs in the same patch and
  state clearly which properties the stubs do not model.
- Affected work: current memory/runtime observability refactor only.

This is a proposal for review. `AGENTS.md`, `TOOLS.md`, and skills were not
modified automatically.
