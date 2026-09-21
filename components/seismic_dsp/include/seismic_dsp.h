#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"


/*
 * These names are the planned processing boundaries. BANDPASS/NOTCH/FFT
 * became real (MAX-133) once they could be built entirely from
 * caller-supplied corner frequencies -- they invent no coefficient of their
 * own, so they cannot violate the project rule against guessed filter
 * bands. FIR/IIR (generic, coefficient-free-form stages) and
 * CROSS_CORRELATION (which needs a second reference buffer this single-
 * buffer API does not yet have) remain descriptive-only placeholders.
 */
enum
{
    SEISMIC_DSP_STAGE_DC_REMOVAL = 1u << 0,
    SEISMIC_DSP_STAGE_WINDOWING = 1u << 1,
    SEISMIC_DSP_STAGE_FIR = 1u << 2,
    SEISMIC_DSP_STAGE_IIR = 1u << 3,
    SEISMIC_DSP_STAGE_BANDPASS = 1u << 4,
    SEISMIC_DSP_STAGE_NOTCH = 1u << 5,
    SEISMIC_DSP_STAGE_FFT = 1u << 6,
    SEISMIC_DSP_STAGE_CROSS_CORRELATION = 1u << 7,
    SEISMIC_DSP_STAGE_NORMALIZATION = 1u << 8,
    SEISMIC_DSP_STAGE_ENERGY_RMS = 1u << 9
};

/* Detrending is the measured-signal refinement of the DC-removal stage. Keep
 * one bit so a future implementation cannot accidentally run both twice. */
#define SEISMIC_DSP_STAGE_DETREND SEISMIC_DSP_STAGE_DC_REMOVAL

#define SEISMIC_DSP_STAGE_ALL ( \
    SEISMIC_DSP_STAGE_DC_REMOVAL | \
    SEISMIC_DSP_STAGE_WINDOWING | \
    SEISMIC_DSP_STAGE_FIR | \
    SEISMIC_DSP_STAGE_IIR | \
    SEISMIC_DSP_STAGE_BANDPASS | \
    SEISMIC_DSP_STAGE_NOTCH | \
    SEISMIC_DSP_STAGE_FFT | \
    SEISMIC_DSP_STAGE_CROSS_CORRELATION | \
    SEISMIC_DSP_STAGE_NORMALIZATION | \
    SEISMIC_DSP_STAGE_ENERGY_RMS \
)


/*
 * CADENCE CONTRACT FOR FIXED-RATE DSP
 *
 * sample_rate_hz is nominal/configured metadata only. It is never proof that
 * every adjacent acquired frame is exactly 1/sample_rate_hz seconds apart.
 * Acquisition timestamps and cadence evidence remain authoritative.
 *
 * Every stateful fixed-Fs stage (currently BANDPASS and NOTCH; a future
 * FIR/IIR/cross-correlation stage inherits the same obligation) is designed
 * once in seismic_dsp_init() from caller-supplied corner frequencies and then
 * carries per-sample filter history across seismic_dsp_process_block() calls
 * so consecutive blocks of one contiguous acquisition filter correctly across
 * the block boundary. The `cadence_contiguous` argument to
 * seismic_dsp_process_block() is how a caller satisfies option 2 of this
 * contract: pass false at the start of a run and immediately after any known
 * collapsed/missing acquisition period to zero all carried filter history
 * before this block is processed, instead of letting stale history leak
 * across a gap it was never told about.
 *
 * A future stage MUST NOT silently process irregular timestamped frames as
 * uniformly sampled data merely because sample_rate_hz is nonzero; it must
 * honor this same flag, or accept only a cadence-valid contiguous window
 * (contract option 1), or resample with an explicitly reviewed algorithm
 * (contract option 3).
 */
#define SEISMIC_DSP_FIXED_RATE_REQUIRES_CADENCE_VALID_INPUT 1u


typedef struct
{
    /* Both corners are required (> 0, low < high < sample_rate_hz/2) the
     * moment SEISMIC_DSP_STAGE_BANDPASS is requested; there is no default.
     * The filter is a single Butterworth (constant 0 dB peak gain) biquad
     * whose center is the geometric mean of the corners and whose Q is
     * center / (high - low) -- both standard, parameter-free conversions of
     * the caller's own corners, not invented constants. */
    float low_cutoff_hz;
    float high_cutoff_hz;

} seismic_dsp_bandpass_config_t;


typedef struct
{
    /* Both fields are required (0 < center_hz < sample_rate_hz/2, q_factor >
     * 0) the moment SEISMIC_DSP_STAGE_NOTCH is requested; there is no
     * default mains frequency baked in here. */
    float center_hz;
    float q_factor;

} seismic_dsp_notch_config_t;


typedef struct
{
    /* Metadata for the measured signal model; zero means unknown. Required
     * (> 0) whenever SEISMIC_DSP_STAGE_BANDPASS or SEISMIC_DSP_STAGE_NOTCH is
     * requested, since both filter designs are a function of it. */
    uint32_t sample_rate_hz;

    /* Requested stages for this configuration. */
    uint32_t requested_stages;

    /* Required only when SEISMIC_DSP_STAGE_BANDPASS is set. */
    seismic_dsp_bandpass_config_t bandpass;

    /* Required only when SEISMIC_DSP_STAGE_NOTCH is set. */
    seismic_dsp_notch_config_t notch;

} seismic_dsp_config_t;


typedef struct
{
    size_t sample_count;
    uint32_t sample_rate_hz;
    uint32_t requested_stages;
    uint32_t placeholder_stages;
    uint32_t completed_stages;
    bool input_passthrough;

    /* True when this call's cadence_contiguous argument was false and at
     * least one stateful filter's carried history was zeroed before this
     * block was processed. */
    bool cadence_reset_applied;

    /* Metadata from the stages actually completed this call. Each field is
     * 0 unless its stage bit is set in completed_stages. */
    float dc_offset_removed; /* SEISMIC_DSP_STAGE_DC_REMOVAL: mean subtracted */
    float energy_rms;        /* SEISMIC_DSP_STAGE_ENERGY_RMS: RMS after any
                               * completed DC removal/bandpass/notch, before
                               * normalization or windowing rescale it */
    float peak_abs;          /* SEISMIC_DSP_STAGE_NORMALIZATION: pre-scale
                               * peak |x| used as the divisor (0 for an
                               * all-zero/degenerate block, in which case the
                               * block is left unscaled rather than divided
                               * by zero) */

    /* SEISMIC_DSP_STAGE_BANDPASS: the corners this call's filter design was
     * actually built from (echoes seismic_dsp_config_t.bandpass). */
    float bandpass_low_cutoff_hz;
    float bandpass_high_cutoff_hz;

    /* SEISMIC_DSP_STAGE_NOTCH: the center/Q this call's filter design was
     * actually built from (echoes seismic_dsp_config_t.notch). */
    float notch_center_hz;
    float notch_q_factor;

    /* SEISMIC_DSP_STAGE_FFT: the strongest non-DC bin in [1, sample_count/2]
     * of a direct DFT computed on the block as it stands after every earlier
     * requested stage ran (diagnostic only -- FFT never mutates the block).
     * fft_dominant_frequency_hz stays 0 when sample_rate_hz is unknown (0)
     * even though fft_dominant_magnitude is still populated, since a bin
     * index cannot be scaled into Hz without a known sample rate. Both stay
     * 0 for a block shorter than 2 samples (no bin to report). */
    float fft_dominant_frequency_hz;
    float fft_dominant_magnitude;

} seismic_dsp_result_t;


/*
 * Configure the process-wide foundation. A NULL config selects a safe empty
 * configuration. No task is created and no sample storage is allocated.
 *
 * Returns ESP_ERR_INVALID_ARG if requested_stages contains an unknown bit,
 * or if SEISMIC_DSP_STAGE_BANDPASS/NOTCH is requested without the matching
 * config fields describing a valid, sub-Nyquist filter -- this component
 * never substitutes a guessed corner/center frequency for a missing one.
 *
 * Resets every stateful filter's carried history to zero, exactly like
 * passing cadence_contiguous=false to the next process_block() call, since a
 * reconfiguration always starts a new acquisition run.
 */
esp_err_t seismic_dsp_init(
    const seismic_dsp_config_t *config
);


/*
 * Process one borrowed block in place for every requested stage this
 * foundation currently implements; any other requested bit is reported in
 * placeholder_stages and left untouched, exactly as before.
 *
 * cadence_contiguous must be false for the first block of a run and for any
 * later block immediately following a known collapsed/missing acquisition
 * period, per the CADENCE CONTRACT above; pass true only when this block is
 * known to directly continue the previous process_block() call's samples at
 * the configured sample_rate_hz with no gap in between.
 *
 * DC_REMOVAL, ENERGY_RMS, NORMALIZATION and WINDOWING are parameter-free and
 * stateless across calls, exactly as before. BANDPASS and NOTCH are real
 * biquad filters designed once in seismic_dsp_init() from the caller's own
 * corner frequencies; they carry per-sample filter history across calls
 * (reset by cadence_contiguous=false) so a signal is filtered correctly
 * across a block boundary rather than independently per block. FFT is a
 * diagnostic-only stage: it never mutates the block and needs no state.
 * FIR/IIR/CROSS_CORRELATION remain unimplemented placeholders (see the stage
 * enum's doc comment for why).
 *
 * Stage order when multiple bits are requested: DC_REMOVAL; then BANDPASS;
 * then NOTCH (so both filters see the DC-removed signal, and NOTCH sees the
 * already band-limited signal); then ENERGY_RMS (measuring the filtered
 * signal's amplitude before anything rescales it); then NORMALIZATION; then
 * WINDOWING; then FFT last, so its diagnostic spectrum reflects every other
 * requested stage's effect on the block.
 */
esp_err_t seismic_dsp_process_block(
    float *input,
    size_t sample_count,
    bool cadence_contiguous,
    seismic_dsp_result_t *result
);
