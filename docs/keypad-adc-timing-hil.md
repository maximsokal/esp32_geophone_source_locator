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

The histogram resolution is 4 microseconds. Therefore `p95` is an upper-bound
bucket value, and the runner deliberately does not invent an accept/reject
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
window ends without the confirmation switch, the report is `BLOCKED`. A
`-LogPath` replay can validate marker shape, source selection, aggregation and
malformed data handling, but it cannot attest to physical key presses or
actual board timing.

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
- timer dispatch-lateness `min/avg/p95/max`, late and overrun counts;
- whether the window was idle or had frequent keypad activity.

Do not compare the diagnostic profile directly with production timing as if
they were identical: the profile adds a 10 ms probe and periodic logging.
Repeat the baseline after any future timing or logging change.
