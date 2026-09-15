#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"


/*
 * These names are the planned processing boundaries. They are deliberately
 * descriptive only until AD7606 + ZF-24HP measurements define the signal
 * model and any filter design parameters.
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
 * CADENCE CONTRACT FOR FUTURE FIXED-RATE DSP
 *
 * sample_rate_hz is nominal/configured metadata only. It is never proof that
 * every adjacent acquired frame is exactly 1/sample_rate_hz seconds apart.
 * Acquisition timestamps and cadence evidence remain authoritative.
 *
 * Before any FIR/IIR/bandpass/notch/FFT/cross-correlation implementation is
 * enabled, its input boundary must do one of the following by reviewed policy:
 *   1. accept only a cadence-valid contiguous window with no known collapsed
 *      acquisition periods and timestamps consistent with that window;
 *   2. reset/split DSP state at each known cadence gap; or
 *   3. resample irregular data with an explicitly reviewed algorithm.
 *
 * A future fixed-Fs stage MUST NOT silently process irregular timestamped
 * frames as uniformly sampled data merely because sample_rate_hz is nonzero.
 * The current placeholder implementation performs no such fixed-rate DSP.
 */
#define SEISMIC_DSP_FIXED_RATE_REQUIRES_CADENCE_VALID_INPUT 1u


typedef struct
{
    /* Metadata for the future measured signal model; zero means unknown. */
    uint32_t sample_rate_hz;

    /* Requested future stages. Current implementation keeps them pending. */
    uint32_t requested_stages;

} seismic_dsp_config_t;


typedef struct
{
    size_t sample_count;
    uint32_t sample_rate_hz;
    uint32_t requested_stages;
    uint32_t placeholder_stages;
    uint32_t completed_stages;
    bool input_passthrough;

} seismic_dsp_result_t;


/*
 * Configure the process-wide foundation. A NULL config selects a safe empty
 * configuration. No task is created and no sample storage is allocated.
 */
esp_err_t seismic_dsp_init(
    const seismic_dsp_config_t *config
);


/*
 * Validate and describe one borrowed block. The current foundation does not
 * modify or retain input; requested stages are reported as placeholders.
 */
esp_err_t seismic_dsp_process_block(
    const float *input,
    size_t sample_count,
    seismic_dsp_result_t *result
);
