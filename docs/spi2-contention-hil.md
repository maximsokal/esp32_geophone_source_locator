# SPI2 contention / UI responsiveness HIL

This is the physical acceptance gate for the shared `SPI2_HOST` path used by
ILI9341 + microSD. It specifically covers the regressions observed on the
2026-09-12 board run: long TFT quiesce windows, sub-1 FPS startup UI, delayed
keypad navigation replay, and storage work that can starve the recorder.

## What the firmware now guarantees structurally

- Keypad Button objects may exist during boot, but `APP_EVENT_KEYPAD` delivery
  is gated until the interactive UI subscribes.
- Keypad navigation that waited in the UI pending queue for more than 250 ms is
  discarded instead of being replayed as a burst after a stall.
- `esp_lvgl_port` remains the sole owner of the LCD
  `on_color_trans_done` callback. The SD guard locks LVGL and drains queued LCD
  color transfers with `esp_lcd_panel_io_tx_param(io, -1, NULL, 0)`.
- Web status refresh reuses the hot object-profile index and reads session
  history through bounded storage windows instead of one whole-history SPI2
  ownership interval.
- 3D scan upload/download never waits on Wi-Fi while holding
  `storage_manager_operation_begin()`: each 4 KiB FAT read/write is a separate
  storage window and socket receive/send happens after the window closes.
- Long scan handlers temporarily run at storage-service priority. The seismic
  recorder now runs at the same priority, while LVGL remains one level higher,
  so continuously-ready bulk HTTP traffic cannot statically outrank recorder
  draining or the display task.

These are code contracts, not proof of physical latency.

## Run 1 — populated-card boot + keypad

Use the normal production firmware and an SD card containing the same order of
magnitude of objects/sessions as the failing run. Start serial logging before
reset.

During the first Web status refresh, repeatedly press all five keypad buttons,
including several presses while SD activity is visible in the log.

Accept only if all of the following are true:

- the UI becomes interactive and remains responsive enough to observe each new
  press without a delayed multi-action burst;
- presses made before interactive readiness do not appear later;
- if a press is delayed past the 250 ms stale-input boundary, the log may show
  `Dropped stale keypad navigation`, but that press must not mutate navigation
  later;
- `WEB_STATUS_CACHE ... mode=cooperative` is present;
- there is no
  `Callback on_color_trans_done was already set and now it was overwritten`;
- inspect `STORAGE_OP_TIMING`: no single operation may cover the complete
  multi-second Web status refresh. Record the worst individual
  `elapsed_us` and `tft_quiesce_us`; do not invent a numeric pass threshold
  until several real-board runs establish the baseline.

The old failing reference was approximately 0.1–0.7 actual FPS during the
initial storage/cache scan, with individual TFT quiesce waits around
0.29–0.33 s. The purpose of this run is to prove that the logical refresh no
longer owns SPI2 as one long operation.

## Run 2 — large scan transfer + interactive TFT

Create or use a valid object, then upload a large saved scan (preferably close
to the current 8 MiB limit) while actively navigating the local TFT UI. Repeat
for scan download.

Accept only if:

- scan upload and download complete correctly;
- navigation does not freeze for the entire network transfer;
- serial shows the scan completion marker with
  `chunked_storage_windows=1`;
- `STORAGE_OP_TIMING` shows many bounded storage windows rather than one window
  with duration comparable to total HTTP transfer time;
- a slow Wi-Fi sender/receiver does not extend an SPI2 lock: pauses in network
  progress must occur between storage windows;
- the LCD callback-overwrite warning remains absent.

Capture total scan transfer time separately from the maximum individual
storage/TFT quiesce time. Total transfer time can be seconds; an individual
storage ownership window must not simply equal that total.

## Run 3 — recorder + scan transfer

This run is blocked until the physical AD7606 5 V rail is repaired. Do not
bypass the existing AD7606 presence/BUSY checks to make the test run.

After INA3221 reports a valid AD7606 rail and acquisition is healthy:

1. start a 500 Hz recording;
2. while recording, upload a large scan and exercise the TFT keypad;
3. repeat with scan download;
4. stop recording normally and inspect the completed session.

Accept only if:

- acquisition remains running;
- recorder reaches normal STOP completion;
- `dropped_frames == 0` for the run;
- the resulting session is readable and complete;
- TFT navigation remains usable and no delayed keypad burst occurs.

This is the decisive test for the former 2048-frame ring-buffer risk: at
500 Hz the ring is only about four seconds deep, so a seconds-long network-held
SPI2 lock is not acceptable.

## ESP timer / Core 0 note

No affinity change is part of this fix. ESP-IDF's `ESP_TIMER_TASK` runs at a
much higher priority than the HTTP server task, and callbacks are serialized;
the existing keypad timing profile remains the correct way to measure dispatch
lateness. Moving the timer task to Core 1 without evidence would instead place
more work on the acquisition core. Re-run `docs/keypad-adc-timing-hil.md` if
this HIL shows unexplained keypad sampling jitter after the SPI2 fixes.

## Evidence to keep

Keep the full UART log for all three runs and record, at minimum: firmware
commit SHA, SD card used, object/session count, scan payload bytes, total scan
transfer time, worst `STORAGE_OP_TIMING elapsed_us`, worst `tft_quiesce_us`,
UI FPS snapshots, recorder `dropped_frames`, and whether any stale-keypad drops
occurred. A run without the physical observations above is not a HIL pass.
