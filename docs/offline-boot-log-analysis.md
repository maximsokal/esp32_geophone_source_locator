# Offline boot/input log analysis

`tools/analyze_boot_responsiveness.py` analyzes an **already captured** passive UART log. It never opens a COM port, flashes the ESP32, resets hardware, or controls power.

This is the preferred post-capture analysis path when the board is flashed manually on COM4 and logs are observed separately on the passive COM6 adapter.

## Run

```powershell
python .\tools\analyze_boot_responsiveness.py .\path\to\uart.log
```

Optional machine-readable output:

```powershell
python .\tools\analyze_boot_responsiveness.py .\path\to\uart.log --json
```

Exit codes:

- `0` — `PASS`: all required evidence exists and no acceptance rule failed.
- `1` — `FAIL`: telemetry proves a real violation (latency limit, stale input, unexpected shutdown, panic, ordering or priority inconsistency, etc.).
- `2` — `BLOCKED`: the log is incomplete for the acceptance decision (for example one of the five buttons was not pressed or recovery/cache completion was not captured).

## Keypad chain

For every physical key code (`1=LEFT`, `2=UP`, `3=DOWN`, `4=RIGHT`, `5=OK`) the analyzer requires all five software stages:

1. `KEYPAD_RAW_TRANSITION ... to=<KEY>` — ADC sample decoded to that physical key.
2. `KEYPAD_BUTTON_PRESS key=<KEY> value=<N>` — official Button debounce emitted `PRESS_DOWN`.
3. `KEYPAD_EVENT_POST key=<N>` — APP_EVENT accepted the input.
4. `KEYPAD_EVENT_DISPATCH key=<N> age_ms=...` — LVGL-side dispatcher consumed a fresh event.
5. `UI_KEY_RESULT key=<N> ...` — the screen handler actually ran and its before/after navigation state was observed.

`UI_KEY_RESULT` proves software state handling; it does **not** prove that physical TFT pixels changed. Visible TFT response remains live-board/manual evidence.

## Startup/storage checks

Default limits:

- `startup_total <= 5000 ms`;
- every reported `OBJECT_RECOVERY_PROGRESS ... elapsed_ms` window `<= 250 ms`;
- background recovery must start after `startup_total`;
- Web status-cache completion must occur after background recovery stops;
- no stale keypad drops;
- no spontaneous `Safe shutdown started`;
- no panic/watchdog/brownout/assert marker.

## web_status_cache priority evidence

Priority snapshots are optional for the boot/keypad PASS because the task may not appear in a sampled top-N snapshot. When a `web_status_cache` sample is present, the analyzer requires:

- `priority_base == 1` by default;
- `inherited == 1` exactly when `priority_current > priority_base`;
- `inherited == 0` when current equals base.

A captured `current=23 base=1 inherited=1` is direct evidence consistent with FreeRTOS mutex priority inheritance. Absence of such a sample is not evidence that inheritance cannot occur.

## Evidence boundary

This analyzer is deterministic parser logic. Its unit tests do not exercise the ESP32, SD card, SPI2 arbitration, FreeRTOS scheduler, keypad ADC ladder, or TFT. Closing the real-board acceptance issues still requires a log captured from the actual device and visual confirmation where required.
