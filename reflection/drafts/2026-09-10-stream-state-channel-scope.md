# Candidate retrospective: transport quality must not erase stream plausibility or application scope

- Evidence: the follow-up audit showed that persistent `ALL_FF`/open-DOUT frames still reported `serial_quality=HEALTHY` with no separate stuck state, and signal-quality rail/clipping metrics were evaluated across all eight AD7606 words even though the seismic buffer consumes four channels.
- Root cause: the first transport/signal split removed quiet/flat patterns from the transport fault state but did not add a third data-plausibility state; the serial metric helper also used the physical ADC word count instead of the application channel count.
- Recurrence count: 1 confirmed episode in this repository; this is not yet a generalized pattern.
- Minimal proposed change: retain transport quality for digital-interference evidence only, add a full-window hysteretic stream state (`NORMAL`/`QUIET`/`STUCK`/`SUSPICIOUS`) with an explicit persistent-stuck flag, and scope signal quality to `SEISMIC_CHANNEL_COUNT` while keeping CH5-CH8 in ADC diagnostics.
- Affected task: persistent AD7606 stream detection and application-channel signal-quality correction on 2026-09-10.
- Validation for this correction: host regression tests cover 500-frame ALL_FF, ALL_00, repeated nonzero, and CH7-only clipping; typecheck, web tests, and the full ESP suite must be recorded in the daily memory log before completion; no flash or monitor is authorized.
