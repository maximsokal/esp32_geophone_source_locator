# Seismic DSP cadence contract

This document fixes the timing contract between seismic acquisition and any future fixed-rate DSP stage.

## Authoritative timing

`sample_rate_hz` is the configured nominal acquisition rate. It is metadata, not proof that every configured period produced a frame.

Per-frame acquisition timestamps and explicit acquisition cadence evidence are authoritative for timing. A collapsed scheduler period is not a recorder drop and must remain distinguishable from `dropped_frames`, read errors, and AD7606 transport health.

## Rule for future fixed-rate processing

A future FIR, IIR, band-pass, notch, FFT, cross-correlation, or other fixed-Fs algorithm must not silently treat adjacent frames as uniformly sampled merely because `sample_rate_hz` is nonzero.

Before such processing is enabled, the reviewed implementation must use one explicit policy:

1. process only cadence-valid contiguous windows with no known collapsed acquisition periods and timestamp geometry consistent with that window;
2. split the stream and reset state around every known cadence gap; or
3. resample irregularly timed data with an explicitly reviewed resampling algorithm.

The current `seismic_dsp` implementation remains a placeholder/pass-through foundation and does not implement any of these algorithms yet.

## Recorder and Web boundary

`samples.bin` already preserves each real frame timestamp and remains the authoritative raw recording. Session cadence metadata records whether configured acquisition periods collapsed during that recording window. Saved envelope points expose both `timestamp_us` and `end_timestamp_us` so reduced waveform buckets preserve their real time span.

The Web UI must render X geometry from real timestamps and must not fabricate uniformly spaced samples across gaps.

## HIL acceptance

For the normal 500 Hz sustained recording baseline:

- cadence evidence missing or invalid => `BLOCKED`;
- any session `collapsed_periods > 0` => `FAIL`;
- valid session cadence with `collapsed_periods == 0` => cadence `PASS`.

This cadence result is independent from recorder drops, AD7606 transport health, raw-SD roundtrip integrity, and throughput margin.
