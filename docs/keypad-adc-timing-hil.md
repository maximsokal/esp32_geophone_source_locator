# Keypad ADC and `ESP_TIMER_TASK` timing HIL

The keypad timing profile measures the real-board cost of the ADC ladder
snapshot and the scheduling effect on the shared ESP-IDF timer task. It is an
opt-in diagnostic profile; production defaults do not enable it because the
probe and its log output add load.

The firmware reports one-second windows:

- `KEYPAD_ADC_TIMING` measures the duration of one broker-owned keypad ADC
  snapshot, including the four useful samples. It reports `min`, weighted
  `avg`, `max`, and a bounded-histogram `p95`.
- `KEYPAD_TIMER_TIMING` measures lateness of a separate 10 ms timer callback
  dispatched by the same `ESP_TIMER_TASK`. `late_count` counts non-zero
  lateness; `overrun_count` counts samples at least one 10 ms period late.

The histogram resolution is 4 microseconds. When the percentile rank remains
inside the representable buckets, `p95` is an upper-bound bucket value. The
normal buckets cover `0..2047` microseconds; samples at or above `2048`
microseconds are excluded from the histogram and counted in `overflow`. The
firmware computes `normal_count = count - overflow` and censors p95 only when
the requested percentile rank is greater than `normal_count`. Therefore a
valid p95 can be `2047` even when a small number of overflow samples exists.
When the rank lands in the overflow tail, the firmware reports
`p95_us=0`, `p95_overflow=1`, and `p95_censored_above_us=2048`. In that case
the numeric p95 is not available; `max_us` remains the measured maximum and
`overflow` reports how many samples were censored. The same fields apply to
timer lateness. The runner deliberately does not invent an accept/reject
threshold before board measurements exist.

## Live procedure

Use separate build directories for the two acquisition backends. The runner
builds the selected profile, checks for live `COM4`/`COM6`, starts a passive
`COM6` monitor, and flashes through `COM4` unless `-SkipFlash` is supplied.

Capture these windows separately:

1. AD7606 with no deliberate keypad activity (baseline timer/ADC load).
2. AD7606 while pressing the keypad repeatedly.
3. ESP ADC bench at 500 Hz while pressing the keypad repeatedly.

Baseline AD7606 window:

```powershell
powershell -NoLogo -NoProfile -ExecutionPolicy Bypass `
  -File tools/run_keypad_timing_hil.ps1 `
  -Source AD7606 `
  -BuildDir build-keypad-timing-ad7606
```

AD7606 pressed window, after the operator is ready to exercise the keys:

```powershell
powershell -NoLogo -NoProfile -ExecutionPolicy Bypass `
  -File tools/run_keypad_timing_hil.ps1 `
  -Source AD7606 `
  -BuildDir build-keypad-timing-ad7606 `
  -RequireFrequentKeypadActivity `
  -ConfirmFrequentKeypadActivity
```

ESP ADC bench pressed window:

```powershell
powershell -NoLogo -NoProfile -ExecutionPolicy Bypass `
  -File tools/run_keypad_timing_hil.ps1 `
  -Source ESP_ADC_BENCH `
  -BuildDir build-keypad-timing-esp-adc `
  -RequireFrequentKeypadActivity `
  -ConfirmFrequentKeypadActivity
```

The `-RequireFrequentKeypadActivity` switch documents the physical condition;
the explicit confirmation records the operator's observation. If the timing
window ends without the confirmation switch, the physical condition is
`NOT_CONFIRMED` and the run is `BLOCKED`. A `-LogPath` replay can validate
marker shape, source selection, aggregation and malformed data handling, but
it cannot attest to physical key presses or actual board timing.

The runner reports `KEYPAD_TIMING_TELEMETRY_RESULT`, which means that the
telemetry was present and internally valid; it is independent of the physical
keypad condition and is not a performance pass. `PHYSICAL_CONDITION` is
`CONFIRMED`, `NOT_CONFIRMED`, or `NOT_REQUIRED`; a replay is never confirmed.
`KEYPAD_TIMING_RUN_RESULT` and the process exit code include a required
physical-attestation block in addition to telemetry failures/blocks. The
runner also prints `PERFORMANCE_ACCEPTANCE: NOT_ESTABLISHED` until real-board
baselines define thresholds. Any non-zero ADC `errors` or malformed telemetry
is an unconditional telemetry `FAIL`. `busy`, `max`, timer lateness,
`overrun_count`, and histogram overflow remain recorded evidence without a
guessed performance threshold.

The default gate requires at least three one-second windows and 100 successful
snapshots. Override these only for a deliberate diagnostic/replay reason with
`-MinimumWindows` and `-MinimumSnapshots`.

## Example replay

The parser can be exercised without a board:

```powershell
powershell -NoLogo -NoProfile -ExecutionPolicy Bypass `
  -File tools/run_keypad_timing_hil.ps1 `
  -Source AD7606 `
  -LogPath .\path\to\captured-keypad-timing.log
```

Replay is parser evidence only. A successful replay must not be reported as
`HIL TESTED` or `PHYSICALLY VERIFIED`.

## Acceptance evidence

Keep the three live logs with the test record. For each log record:

- selected backend and configured 500 Hz rate;
- ADC snapshot `min/avg/p95/max`, sample count, busy/error count and overflow;
  record `p95_overflow` and `p95_censored_above_us` when p95 is not available;
- timer dispatch-lateness `min/avg/p95/max`, late and overrun counts, with the
  same p95-censor fields;
- whether the window was idle or had frequent keypad activity.

Do not compare the diagnostic profile directly with production timing as if
they were identical: the profile adds a 10 ms probe and periodic logging.
Repeat the baseline after any future timing or logging change.
