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

## Additional ADC ownership correction

### Evidence

- The audit's interleaving was real: the service's discard/useful pair could be interrupted by the vendor ADC Button backend because that backend retained and read the same ADC1 handle outside `adc_service`.
- The keypad now creates the official Button state-machine objects with keypad-owned `button_driver_t` adapters. Every level callback obtains the ladder sample through `adc_service_read_average_raw()`; a short callback-local cache lets the five handles share one broker-owned sample without multiplying conversions.
- The public `adc_service_handle()` accessor and all keypad use of the vendor `button_adc` backend were removed. Host coverage now exercises the custom callbacks and asserts zero direct ADC reads; the final native host suite passed 38/38.
- The post-change production build passed with image `0x1fdd30` and `0x2022d0` app free; the isolated Insights profile passed with image `0x20d360` and `0x1f2ca0` app free. The full local suite before the retry-worker correction exited 0: Host C 38/38, Python 74 passed plus 1 skip, Web Console 38/38, Web XR 582/582, typecheck, Unity build, and production build.

### Root cause

- The previous settling mutex protected only service calls. It did not establish ownership because a third-party Button driver still held a borrowed ADC1 handle and could issue a conversion between the discard and useful conversion.

### Recurrence count

- One confirmed occurrence in this task. This is enough to require the repository-local ownership contract, but not enough evidence for a generalized workflow or skill change.

### Proposed minimal rule

- Treat the ADC1 handle as private to `adc_service`; all application and vendor-adapter reads must cross the service/broker API. Add a host regression that models every ADC consumer and rejects direct reads from each adapter. Keep physical ADC/button behavior explicitly pending until target UART/HIL evidence exists.

## Additional Insights retry-worker correction

### Evidence

- The LOW/watchlist concern was also corrected: the APP_EVENT connection handler now only calls `xTaskNotifyGive()` and returns. A dedicated service-core worker consumes the notification and performs the potentially non-trivial `esp_insights_init()` retry.
- The worker's priority and stack are centralized in `app_task_config.h`, the optional component explicitly requires `freertos`, and the source contract proves that the APP_EVENT handler does not call `insights_bridge_attempt_init()` directly.
- After the correction, the production build still passed with image `0x1fdd30` and `0x2022d0` app free. The isolated Insights profile passed with image `0x20d360` and `0x1f2ca0` app free; `idf.py size` and `idf.py size-components` exited 0 for both profiles. Focused architecture/liveness contracts passed 10/10.
- The final `tools/run_suite.ps1 -Suite All -ProductionBuildDir build-refactor-final` exited 0 after the correction: Host C 38/38, Python 74 passed plus 1 skip, Web Console 38/38, Web XR 582/582, typecheck, Unity build, and production build passed; HIL was skipped.

### Root cause

- The prior retry had been moved off the default `sys_evt` task but still ran synchronously on the shared APP_EVENT loop, which also services UI and application events.

### Recurrence count

- One confirmed occurrence in this task. This is repository-local evidence for keeping event callbacks notification-only, not enough evidence for a generalized skill change.

### Proposed minimal rule

- Keep shared event handlers bounded: validate the event, set/notify state, and return. Run optional initialization, diagnostics setup, or network-facing work in a dedicated worker with an explicit task-policy entry and a source contract that excludes the heavy call from the handler.

## Additional timer-safe ADC and atomic-state correction

### Evidence

- The ownership refactor removed the second ADC1 owner but left the Button
  level callback able to wait forever on the broker mutex from the shared
  `ESP_TIMER_TASK`. The keypad callback now calls
  `adc_service_try_read_average_raw()` with zero wait ticks and immediately
  retains its last decoded state on `ESP_ERR_TIMEOUT`.
- The callback records a separate failed-attempt timestamp so the remaining
  logical Button handles do not retry the broker during the same cache window.
  A successful read records `s_adc_cache_time_us` after the broker call. Host
  coverage models both contention and a 1.5 ms fake read and passed in the
  complete 38/38 native suite.
- `insights_bridge` now stores `s_initialized` and `s_retry_pending` as
  `atomic_bool`, using acquire loads and release stores across the retry worker
  and public/main contexts. The source contract requires these operations.

### Root cause

- The first ownership contract covered who may issue ADC1 conversions, but not
  whether a system-timer callback may wait for that owner. The later Insights
  worker correction introduced cross-context state without completing its
  visibility contract.

### Recurrence count

- One confirmed occurrence of each issue in this task. This is repository-local
  evidence for the timer-callback and atomic-state contracts, not enough for a
  generalized workflow or skill change.

### Proposed minimal rule

- Callbacks running on shared timer/event infrastructure must use bounded or
  zero-wait broker APIs and preserve the last valid state on contention. Any
  state shared with a worker must use an explicit synchronization primitive;
  source contracts should assert both the non-blocking call path and the
  publication mechanism.

## Additional keypad timing watchlist instrumentation

### Evidence

- The remaining concern was load-sensitive rather than an ownership race:
  `ESP_TIMER_TASK` performs the broker-owned discard plus useful keypad ADC
  conversions, and no measured board duration or timer jitter was available.
- Added an opt-in target profile with bounded one-second ADC and same-task
  timer-lateness histograms. The runner validates source markers, minimum
  windows/snapshots, malformed percentile data, and refuses to treat a log
  replay as proof of physical key activity.
- Both isolated target profiles compiled: AD7606 image `0x1fe6a0`, ESP ADC
  bench-at-500-Hz image `0x1fc8a0`. The final local suite passed Host C 38/38,
  Python 80 plus 1 skip, web 38/38 and 582/582, typecheck, Unity, and
  production build; the focused timing contract passed 6/6 after a runner
  parameter test correction.

### Root cause

- Static code review could establish bounded nonblocking ownership but could
  not establish the real ESP32 polling-conversion duration or the shared
  timer-task scheduling effect. A production-enabled probe would itself alter
  that load, so the measurement must remain an isolated HIL profile.

### Recurrence count

- One confirmed watchlist item in this task. This supports a repository-local
  measurement contract, not a generalized optimization rule.

### Proposed minimal rule

- For embedded timer/load watchlists, add only opt-in instrumentation with
  bounded accounting, explicit parser/replay contracts, separate backend
  profiles, and no guessed threshold. Keep `COMPILES`, `HIL TESTED`, and
  `PHYSICALLY VERIFIED` separate; do not claim board numbers until live serial
  evidence exists.

### Affected task

ESP32 geophone keypad ADC1 ownership/timer-safety follow-up; physical timing
remains pending because COM4/COM6 are not live.

## Additional timing telemetry validity correction

### Evidence

- The timing audit found that the bounded 512-bucket histogram collapsed all
  durations at or above 2048 us into its last bucket, while the old formatter
  still presented 2048 us as a p95 upper bound. The firmware now reports a
  censored p95 as `p95_us=0`, `p95_overflow=1`, and
  `p95_censored_above_us=2048`; actual `max_us` and histogram overflow counts
  remain available. The same contract is applied to timer lateness.
- The HIL runner now reports `KEYPAD_TIMING_TELEMETRY_RESULT` separately from
  `PERFORMANCE_ACCEPTANCE: NOT_ESTABLISHED`. It has no invented performance
  thresholds; malformed telemetry and non-zero ADC errors fail, while
  unthresholded performance observations remain recorded.
- Regression evidence passed: focused timing tests `8/8`, Host C `38/38`,
  full Python `82 passed + 1 skipped`, and full `Suite All` exited 0. Enabled
  target profiles compiled with the expected diagnostics/source selections:
  AD7606 image `0x1fe7a0` and ESP ADC bench-at-500-Hz image `0x1fc990`.

### Root cause

- The metric implementation treated a censored terminal histogram bucket as a
  normal bucket, and the runner used a performance-sounding PASS result before
  real-board baseline thresholds existed.

### Recurrence count

- One confirmed occurrence in this task. This supports a repository-local
  censored-metric and pre-baseline HIL naming rule, not a generalized skill
  change.

### Proposed minimal rule

- Any bounded metric whose percentile can land in an overflow bucket must
  publish validity plus the censoring bound separately from the numeric value.
  Before physical baselines exist, HIL output should certify telemetry
  integrity only; performance acceptance must remain explicitly unestablished.

### Affected task

ESP32 geophone keypad timing HIL follow-up; physical timing acceptance remains
pending until live COM4/COM6 runs produce board measurements.

## Additional timing histogram boundary and HIL-result separation correction

### Evidence

- The follow-up audit found that the terminal 4-us histogram bucket mixed valid
  `2044..2047`-us values with `>=2048`-us overflow values. The implementation
  now excludes overflow values from all 512 normal buckets and computes
  `normal_count=count-overflow`; p95 is censored only when its rank is greater
  than `normal_count`. Native C regressions cover a valid `2047`-us p95, a
  genuinely censored overflow tail, and a valid p95 with one overflow sample
  out of 100.
- The runner previously combined the physical keypad-attestation check into its
  telemetry result. It now reports telemetry validity separately from
  `PHYSICAL_CONDITION`, keeps `PERFORMANCE_ACCEPTANCE` explicitly
  `NOT_ESTABLISHED`, and uses `KEYPAD_TIMING_RUN_RESULT` plus the exit code for
  the complete gate.
- Verification passed Host C `39/39`, Python `85 passed + 1 skipped`, the full
  web/typecheck/Unity/production suite, and both enabled timing target profiles.
  No COM4/COM6 ports were live, so no physical timing or keypad evidence was
  claimed.

### Root cause

- The percentile representation treated a right-censored tail as if it were a
  normal terminal bucket, and the runner's single aggregate status conflated
  telemetry integrity with a separately required manual physical condition.

### Recurrence count

- One confirmed occurrence in this task. This supports a repository-local
  censored-metric and HIL-status separation rule, not a generalized skill
  change.

### Proposed minimal rule

- Keep normal histogram support disjoint from overflow/censored samples and
  make percentile validity depend on the requested rank, not bucket index.
  Name each HIL status after exactly what it proves; use a separate run gate for
  physical attestation and keep performance acceptance unestablished until
  measured baselines exist.

### Affected task

ESP32 geophone keypad timing HIL follow-up; physical timing acceptance remains
pending until live COM4/COM6 runs produce board measurements.
