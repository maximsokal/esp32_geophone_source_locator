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

As of MAX-133, `seismic_dsp`'s BANDPASS and NOTCH stages are real, stateful biquad filters and implement policy 2: `seismic_dsp_process_block()` takes an explicit `cadence_contiguous` argument, and the caller must pass `false` for the first block of a run and for any block immediately following a known collapsed/missing acquisition period, which zeroes both filters' carried history before that block is processed. DC_REMOVAL, ENERGY_RMS, NORMALIZATION and WINDOWING stay stateless (no cross-block history to leak, so the contract is satisfied trivially), and FFT is diagnostic-only and stateless. FIR, IIR (as generic, coefficient-free-form stages) and CROSS_CORRELATION remain an unimplemented placeholder/pass-through and do not yet implement any of these three policies.

## Recorder and Web boundary

`samples.bin` already preserves each real frame timestamp and remains the authoritative raw recording. Session cadence metadata records whether configured acquisition periods collapsed during that recording window. Saved envelope points expose both `timestamp_us` and `end_timestamp_us` so reduced waveform buckets preserve their real time span.

The Web UI must render X geometry from real timestamps and must not fabricate uniformly spaced samples across gaps.

## HIL acceptance

For the normal 500 Hz sustained recording baseline:

- cadence evidence missing or invalid => `BLOCKED`;
- any session `collapsed_periods > 0` => `FAIL`;
- valid session cadence with `collapsed_periods == 0` => cadence `PASS`.

This cadence result is independent from recorder drops, AD7606 transport health, raw-SD roundtrip integrity, and throughput margin.
