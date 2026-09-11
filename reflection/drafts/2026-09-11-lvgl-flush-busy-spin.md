# Candidate retrospective: LVGL flush busy-spin on CPU0

- Evidence: the managed LVGL 9 refresh path used a tight `while (disp->flushing)` fallback when no flush-wait callback was installed. The application used asynchronous SPI LCD DMA, so CPU0 could spin while waiting for the DMA completion. The tracked UI override now bridges the panel-I/O ISR to a static semaphore, waits in LVGL's task context, records `UI_FLUSH_PERF`, and drains DMA before teardown. The ESP-IDF 5.5.4 production build and full local suite passed after the change.
- Root cause: the display driver provided asynchronous completion but the LVGL integration did not provide a blocking wait callback, leaving LVGL's fallback polling loop active.
- Recurrence count: one confirmed episode in this task. This is a candidate rule, not a generalized rule yet.
- Proposed minimal rule/tool change: when diagnosing embedded CPU saturation around asynchronous peripherals, inspect the third-party consumer's completion/wait path and measure the wait itself before changing task priorities or buffer sizes.
- Affected prior work: CPU0/TFT performance diagnosis and LVGL/SPI display lifecycle. No managed component or project instruction file was modified.
