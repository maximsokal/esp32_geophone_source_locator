# Candidate retrospective: follow-up profile and timing contracts

## Evidence

- The first opt-in Insights build reported a green build while reusing the
  shared root `sdkconfig`; generated configuration showed
  `CONFIG_APP_ESP_INSIGHTS` was actually disabled.
- The isolated profile then exposed a real enabled-branch compile error:
  `esp_diagnostics_metrics.h` was not included before the metrics API was
  used. Adding the official header fixed the profile build.
- The first host build after timing instrumentation failed only under `/W4
  /WX` because the silent `ESP_LOGI` test stub made the elapsed-time local
  appear unused. An explicit `(void)` keep-live fixed the test seam.
- A focused documentation contract also caught that the plan named the
  timing marker but not its `debounced_elapsed_ms` field; the contract and
  documentation were corrected.
- The added replay regression initially assumed an exact number of padding
  spaces in the human-readable timing row. The runner correctly returned
  four `cannot be used with -LogPath` blocks; the test was corrected to assert
  the semantic row name instead of presentation width.
- A prior daily-log entry carried a different dependency-lock hash. The final
  direct `Get-FileHash dependencies.lock -Algorithm SHA256` check, together
  with a clean `git diff -- dependencies.lock`, established the current hash
  as `F3EEABDEEAB499FC85BD7106303685CBFB99533B51E9382B2832068D44787CEB`;
  the append-only log records the correction without rewriting history.

## Root cause

- Optional compile-time branches were not initially verified against an
  isolated generated configuration.
- PowerShell variable names are case-insensitive, so a local `$sdkconfig`
  variable could overwrite the `[string]$Sdkconfig` parameter.
- Feature-branch headers and host logging stubs were not exercised together
  before the first full verification pass.
- The lock hash was copied from stale intermediate evidence instead of being
  recomputed at the final verification boundary.

## Recurrence count

Each failure mode has one confirmed occurrence in this task. This is evidence
for a local workflow rule, not enough evidence for a generalized repository-
wide skill change.

## Proposed minimal rule

For every optional ESP-IDF profile, use a profile-specific `SDKCONFIG` path,
assert the generated flags and any test provider object, and compile the
enabled source branch. Avoid case-only PowerShell parameter/local names. Run
host warning-as-error tests with silent logging stubs before claiming the
profile or instrumentation contract green. Replay tests should validate
statuses and diagnostic meaning, not incidental table alignment whitespace.
Recompute integrity hashes at the final verification boundary rather than
carrying values forward from an earlier pass.

## Affected task

ESP32 geophone architecture-refactor follow-up: ADC1 external-borrower
settling, optional ESP Insights startup/privacy profile, and raw GPIO21 power
button timing acceptance contract.
