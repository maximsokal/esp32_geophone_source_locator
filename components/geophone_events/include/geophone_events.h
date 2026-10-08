#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The boundary between seismic_dsp's preprocessed blocks and localization's
 * TDOA observations: per-channel candidate detection and arrival-time
 * estimation.
 *
 * Every function here is pure and stateless -- callers own the sample
 * buffer and all configuration. Per the MAX-132 project rule ("No production
 * implementation should start from guessed filter bands or propagation
 * velocity; measured ZF-24HP/AD7606 data and explicit method configuration
 * must remain authoritative"), no function below picks a default window
 * length, threshold or ratio: every config struct must be filled in by the
 * caller from measured/reviewed values, never invented here.
 *
 * Callers are expected to have already run seismic_dsp's DC-removal stage
 * (or equivalent) on the buffer before calling into this component; nothing
 * here assumes or re-derives a DC offset.
 */


/* ---- Energy/RMS trigger baseline ---------------------------------------
 * A simple, deliberately unsophisticated detector kept independently
 * testable/comparable against the adaptive STA/LTA detector below, per the
 * ticket's "separable and comparable" requirement. */

typedef struct
{
    /* Leading samples of the buffer assumed to be pre-event background. */
    size_t noise_window_samples;

    /* Sliding window length used to scan for the trigger. */
    size_t trigger_window_samples;

    /* Trigger fires when windowed RMS >= threshold_multiplier * noise RMS. */
    float threshold_multiplier;

} geophone_energy_trigger_config_t;


typedef struct
{
    bool found;
    size_t trigger_index; /* start of the first window that exceeded threshold */
    float noise_rms;
    float trigger_rms;

} geophone_energy_trigger_result_t;


bool geophone_event_energy_trigger_detect(
    const float *samples,
    size_t sample_count,
    const geophone_energy_trigger_config_t *config,
    geophone_energy_trigger_result_t *result
);


/* ---- STA/LTA adaptive onset detector ------------------------------------ */

typedef struct
{
    size_t sta_window_samples;
    size_t lta_window_samples; /* must be > sta_window_samples */
    float trigger_ratio;       /* STA/LTA >= trigger_ratio fires */

} geophone_sta_lta_config_t;


typedef struct
{
    bool found;
    size_t trigger_index; /* start of the STA window at first trigger */
    float ratio_at_trigger;

} geophone_sta_lta_result_t;


bool geophone_event_sta_lta_detect(
    const float *samples,
    size_t sample_count,
    const geophone_sta_lta_config_t *config,
    geophone_sta_lta_result_t *result
);


bool geophone_event_sta_lta_detect_after(
    const float *samples,
    size_t sample_count,
    const geophone_sta_lta_config_t *config,
    size_t minimum_trigger_index,
    geophone_sta_lta_result_t *result
);


/* ---- First-arrival refinement around a coarse trigger ------------------- */

typedef struct
{
    size_t lookback_samples;   /* how far before the coarse trigger to search */
    size_t lookahead_samples;  /* how far after it, to locate the local peak */
    float amplitude_fraction;  /* refine to the first sample >= this fraction
                                 * of the local peak found in the search window
                                 * that belongs to the peak's own burst (see
                                 * geophone_event_refine_arrival()) */

} geophone_arrival_refine_config_t;


typedef struct
{
    bool found;
    size_t refined_index;
    /* MAX-209: linear-interpolated threshold crossing between
     * refined_index - 1 and refined_index, in samples, within (-1, 0]; the
     * sub-sample onset is refined_index + refined_fraction. 0 when the
     * crossing sample is the first of the search window. */
    float refined_fraction;
    float local_peak_abs;

} geophone_arrival_refine_result_t;


bool geophone_event_refine_arrival(
    const float *samples,
    size_t sample_count,
    size_t coarse_trigger_index,
    const geophone_arrival_refine_config_t *config,
    geophone_arrival_refine_result_t *result
);


/* ---- Pairwise cross-correlation delay refinement ------------------------ */

typedef struct
{
    size_t max_lag_samples; /* search delays in [-max_lag, +max_lag] */

} geophone_cross_correlation_config_t;


typedef struct
{
    bool found;
    int32_t delay_samples;  /* positive: channel_b lags channel_a */
    float peak_correlation; /* normalized coefficient, ideally in [-1, 1] */
    /* MAX-209: sub-sample delay from a parabola through the correlation at
     * delay_samples - 1, delay_samples, delay_samples + 1 (equal to
     * delay_samples at the edge of the lag range). */
    double delay_refined_samples;

} geophone_cross_correlation_result_t;


bool geophone_event_cross_correlate(
    const float *channel_a,
    const float *channel_b,
    size_t sample_count,
    const geophone_cross_correlation_config_t *config,
    geophone_cross_correlation_result_t *result
);


/* ---- SNR / accept-reject gate -------------------------------------------
 * "Reject/flag low-SNR or ambiguous picks rather than inventing a
 * timestamp": every caller-facing pick below reports an explicit rejection
 * status instead of silently returning an unqualified arrival time. */

float geophone_event_compute_snr(
    const float *samples,
    size_t sample_count,
    size_t noise_window_samples,
    size_t arrival_index,
    size_t signal_window_samples
);


typedef struct
{
    float minimum_snr;

} geophone_pick_quality_config_t;


bool geophone_event_pick_is_acceptable(
    float snr,
    const geophone_pick_quality_config_t *config
);


/* ---- Combined per-channel pick ------------------------------------------
 * Runs STA/LTA -> first-arrival refinement -> SNR gate as one reviewed
 * pipeline. The energy-trigger baseline and cross-correlation refinement
 * above remain separately callable for comparison/inter-channel work; they
 * are not folded into this fixed pipeline. */

typedef enum
{
    GEOPHONE_PICK_METHOD_STA_LTA_REFINED = 0

} geophone_pick_method_t;


typedef enum
{
    GEOPHONE_PICK_ACCEPTED = 0,
    GEOPHONE_PICK_REJECTED_NO_TRIGGER,
    GEOPHONE_PICK_REJECTED_NO_REFINEMENT,
    GEOPHONE_PICK_REJECTED_LOW_SNR,
    /* MAX-209: accepted on its own, but its arrival time is physically
     * incompatible with the other channels' picks (a time difference larger
     * than the sensor separation allows at the propagation speed) -- most
     * likely a different event, e.g. a footstep next to one sensor. */
    GEOPHONE_PICK_REJECTED_INCONSISTENT,
    GEOPHONE_PICK_REJECTED_SATURATED,
    GEOPHONE_PICK_REJECTED_AMBIGUOUS

} geophone_pick_status_t;


typedef struct
{
    size_t noise_window_samples;

    size_t sta_window_samples;
    size_t lta_window_samples;
    float sta_lta_trigger_ratio;

    size_t refine_lookback_samples;
    size_t refine_lookahead_samples;
    float refine_amplitude_fraction;

    size_t snr_signal_window_samples;
    float minimum_snr;

} geophone_channel_pick_config_t;


typedef enum
{
    GEOPHONE_PICK_REASON_NONE = 0,
    GEOPHONE_PICK_REASON_SATURATED = 1,
    GEOPHONE_PICK_REASON_AMBIGUOUS = 2

} geophone_pick_reason_t;


/* Stable lowercase reason names for logs and later persisted pick evidence. */
const char *geophone_pick_reason_name(geophone_pick_reason_t reason);


typedef struct
{
    geophone_pick_status_t status;
    geophone_pick_reason_t reason;
    geophone_pick_method_t method;
    size_t arrival_sample_index;
    /* MAX-209: sub-sample correction in (-1, 0]; the onset is at
     * arrival_sample_index + arrival_subsample_offset samples. */
    float arrival_subsample_offset;
    float arrival_time_s; /* (arrival_sample_index + offset) / sample_rate */
    float snr;
    size_t uncertainty_samples; /* pick resolution proxy: the refine search span */

} geophone_channel_pick_t;


/*
 * Returns true only when status == GEOPHONE_PICK_ACCEPTED. A false return
 * still fills *result with the specific rejection status (and, where a
 * coarse/refined index exists, diagnostic arrival_sample_index/snr) so
 * callers and logs can see *why* a channel produced no usable pick instead
 * of only that it failed.
 */
bool geophone_event_pick_channel_arrival(
    const float *samples,
    size_t sample_count,
    uint32_t sample_rate_hz,
    const geophone_channel_pick_config_t *config,
    geophone_channel_pick_t *result
);


/* Check the original int16 analysis window before unit conversion or
 * filtering. A window is saturated only when it contains at least 8 exact
 * INT16_MIN/INT16_MAX samples and those rails make up at least 0.5% of the
 * full window (ceil(sample_count * 0.005)). Pass the resulting compact flag
 * to the picker below while retaining the normal float trace for detection. */
bool geophone_event_raw_i16_window_is_saturated(
    const int16_t *raw_samples,
    size_t sample_count
);


bool geophone_event_pick_channel_arrival_with_saturation(
    const float *samples,
    size_t sample_count,
    uint32_t sample_rate_hz,
    const geophone_channel_pick_config_t *config,
    bool raw_i16_saturated,
    geophone_channel_pick_t *result
);


/* ---- Secondary (S-wave) arrival after an accepted P pick ---------------
 * PSAFUVS locates without a velocity from P AND S arrivals. On a
 * single-component geophone the S wave is a second, later energy increase
 * on top of the P coda: an STA/LTA whose long window lies entirely after
 * the P onset (so it measures the coda, never the pre-event noise) fires on
 * it. The trigger is then refined with the parameter-free AIC onset picker
 * (Maeda 1985) over the coda + trigger window. Every window and threshold
 * is caller input (MAX-132); the S pick is reported with
 * uncertainty_samples = sta_window_samples and no sub-sample offset. */
typedef struct
{
    /* The S onset is searched only in [P + min, P + max] samples. */
    size_t min_separation_samples;
    size_t max_separation_samples;
    size_t sta_window_samples;
    size_t lta_window_samples; /* > sta; lies after the P onset */
    float sta_lta_trigger_ratio;
    /* RMS of the S window over RMS of the P coda just before it. */
    float minimum_snr;

} geophone_secondary_pick_config_t;

bool geophone_event_pick_secondary_arrival(
    const float *samples,
    size_t sample_count,
    uint32_t sample_rate_hz,
    size_t primary_arrival_index,
    const geophone_secondary_pick_config_t *config,
    geophone_channel_pick_t *result
);
