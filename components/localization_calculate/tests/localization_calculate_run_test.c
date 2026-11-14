/*
 * MAX-168 review follow-up: localization_calculate_run() -- the orchestrator
 * behind "Calculate source" for a finalized recording -- was never actually
 * executed by any test. The HTTP layer stubs it out entirely and the Python
 * contract test only greps source text. This host test links and runs the
 * REAL localization_calculate.c against fakes for its I/O boundary
 * (seismic_recorder's raw/geometry readers, localization_run_store, the
 * scan occupancy file, and the esp_timer clock) with synthesized raw
 * samples from a known source, and checks the concrete contracts MAX-168
 * describes: dimensionality policy, method fan-out with reasons for
 * unsolved methods, no fabricated Z at PLANAR_2D, fail-closed error
 * propagation, run-store write-failure handling, the time budget, and the
 * scan-constraint "not applied" fallback.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "localization_calculate.h"
#include "localization_calculate_failure_class_test_support.h"
#include "localization_calculate_policy.h"
#include "localization_dimensionality_policy.h"
#include "localization_run_store.h"
#include "object_store.h"
#include "psram_model.h"
#include "scan_occupancy.h"
#include "seismic_recorder.h"
#include "seismic_buffer.h"
#include "storage_manager.h"


/* ==== Fake clock (esp_timer_get_time) ==================================== */

static int64_t g_now_us = 1000000;

int64_t esp_timer_get_time(void)
{
    return g_now_us;
}


/* ==== Fake storage_manager arbitration (never touches real hardware) ===== */

esp_err_t storage_manager_operation_begin(void)
{
    return ESP_OK;
}

void storage_manager_operation_end(void)
{
}


/* ==== Fake seismic_recorder read functions ================================
 * localization_calculate_run() never learns raw samples or frozen geometry
 * any other way, so these two functions are the entire I/O seam this test
 * needs to control. */

static object_store_geometry_t g_fixture_geometry;
static seismic_frame_t *g_fixture_frames;
static size_t g_fixture_frame_count;
static seismic_recorder_session_summary_t g_fixture_summary;
static size_t g_fixture_gap_index;
static size_t g_fixture_nonmonotonic_index;
static size_t g_fixture_rail_counts[OBJECT_STORE_POD_COUNT];
static esp_err_t g_fixture_geometry_err;
static esp_err_t g_fixture_raw_err;
static size_t g_geometry_read_calls;
static size_t g_localization_geometry_calls; /* the run must read its geometry through the localization one */
static size_t g_summary_read_calls;
static size_t g_range_read_calls;
static size_t g_range_read_max_frames;

esp_err_t seismic_recorder_read_session_geometry(
    const char *object_id,
    const char *session_id,
    object_store_geometry_t *geometry
)
{
    (void)object_id;
    (void)session_id;
    ++g_geometry_read_calls;
    if (g_fixture_geometry_err != ESP_OK) {
        return g_fixture_geometry_err;
    }
    *geometry = g_fixture_geometry;
    return ESP_OK;
}

/* Which layout the fake hands the localization: the Channel Sounding one, or registered AprilTag centres. */
static seismic_recorder_localization_geometry_source_t g_fixture_geometry_source =
    SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_CHANNEL_SOUNDING;

esp_err_t seismic_recorder_read_session_localization_geometry(
    const char *object_id,
    const char *session_id,
    object_store_geometry_t *geometry,
    seismic_recorder_localization_geometry_source_t *source,
    esp_err_t *registered_status
)
{
    ++g_localization_geometry_calls;
    if (source != NULL) *source = g_fixture_geometry_source;
    if (registered_status != NULL) *registered_status = ESP_OK;
    return seismic_recorder_read_session_geometry(object_id, session_id, geometry);
}

esp_err_t seismic_recorder_get_session_summary(
    const char *object_id,
    const char *session_id,
    seismic_recorder_session_summary_t *summary_out
)
{
    (void)object_id;
    (void)session_id;
    ++g_summary_read_calls;
    if (g_fixture_raw_err != ESP_OK) {
        return g_fixture_raw_err;
    }
    if (summary_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *summary_out = g_fixture_summary;
    return ESP_OK;
}

esp_err_t seismic_recorder_read_session_range_ex(
    const char *object_id,
    const char *session_id,
    uint64_t start_index,
    size_t max_frames,
    seismic_frame_t *frames,
    uint64_t *source_sample_indices,
    bool *gap_before,
    size_t *frame_count_out,
    seismic_recorder_session_summary_t *summary_out,
    seismic_recorder_raw_window_stats_t *raw_stats_out
)
{
    (void)object_id;
    (void)session_id;
    ++g_range_read_calls;
    g_range_read_max_frames = max_frames;
    if (g_fixture_raw_err != ESP_OK) {
        return g_fixture_raw_err;
    }
    if (
        frames == NULL || frame_count_out == NULL || start_index > g_fixture_frame_count ||
        max_frames == 0U
    ) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t remaining = g_fixture_frame_count - (size_t)start_index;
    const size_t count = remaining < max_frames ? remaining : max_frames;
    memcpy(frames, g_fixture_frames + (size_t)start_index, sizeof(*frames) * count);
    for (size_t i = 0U; i < count; ++i) {
        const size_t sample_index = (size_t)start_index + i;
        if (source_sample_indices != NULL) {
            source_sample_indices[i] = (uint64_t)sample_index;
        }
        if (gap_before != NULL) {
            gap_before[i] = sample_index == g_fixture_gap_index;
        }
        if (sample_index == g_fixture_nonmonotonic_index && i > 0U) {
            frames[i].timestamp_us = frames[i - 1U].timestamp_us - 1;
        }
    }
    *frame_count_out = count;
    if (raw_stats_out != NULL) {
        memset(raw_stats_out, 0, sizeof(*raw_stats_out));
        raw_stats_out->samples_examined = count;
        for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
            raw_stats_out->rail_sample_counts[ch] = g_fixture_rail_counts[ch];
        }
    }
    if (summary_out != NULL) {
        *summary_out = g_fixture_summary;
    }
    return ESP_OK;
}


esp_err_t seismic_recorder_scan_session(
    const char *object_id, const char *session_id, seismic_frame_t *buffer,
    size_t capacity, seismic_recorder_scan_callback_t callback, void *context
)
{
    (void)object_id;
    (void)session_id;
    if (g_fixture_raw_err != ESP_OK) return g_fixture_raw_err;
    if (buffer == NULL || capacity == 0U || callback == NULL) return ESP_ERR_INVALID_ARG;
    for (size_t start = 0U; start < g_fixture_frame_count;) {
        size_t count = g_fixture_frame_count - start;
        if (count > capacity) count = capacity;
        if (g_fixture_gap_index > start && g_fixture_gap_index - start < count) {
            count = g_fixture_gap_index - start;
        }
        memcpy(buffer, g_fixture_frames + start, count * sizeof(*buffer));
        if (g_fixture_nonmonotonic_index >= start &&
            g_fixture_nonmonotonic_index - start < count && g_fixture_nonmonotonic_index > 0U) {
            const size_t i = g_fixture_nonmonotonic_index - start;
            buffer[i].timestamp_us = g_fixture_frames[g_fixture_nonmonotonic_index - 1U].timestamp_us - 1;
        }
        const esp_err_t err = callback(buffer, count, (uint64_t)start, start == g_fixture_gap_index, context);
        if (err != ESP_OK) return err;
        start += count;
    }
    return ESP_OK;
}


/* ==== Fake localization_run_store: records every write ==================== */

#define MAX_RECORDED_WRITES 32U

static localization_run_record_t g_recorded[MAX_RECORDED_WRITES];
static size_t g_recorded_count;
static size_t g_write_call_count;
static size_t g_pick_write_calls;
static localization_pick_record_t g_pick_record;

static bool g_fail_write_at_set;
static size_t g_fail_write_at_index;
static esp_err_t g_fail_write_err;

esp_err_t localization_run_store_write(
    const char *object_id,
    const char *session_id,
    const localization_run_record_t *record
)
{
    (void)object_id;
    (void)session_id;
    const size_t call_index = g_write_call_count++;
    if (g_recorded_count < MAX_RECORDED_WRITES) {
        g_recorded[g_recorded_count++] = *record;
    }
    if (g_fail_write_at_set && call_index >= g_fail_write_at_index) {
        return g_fail_write_err;
    }
    return ESP_OK;
}

esp_err_t localization_run_store_write_picks(
    const char *object_id,
    const char *session_id,
    const localization_pick_record_t *record
)
{
    (void)object_id;
    (void)session_id;
    ++g_pick_write_calls;
    g_pick_record = *record;
    return ESP_OK;
}


static void reset_fakes(void)
{
    g_now_us = 1000000;
    memset(&g_fixture_geometry, 0, sizeof(g_fixture_geometry));
    g_fixture_geometry_err = ESP_OK;
    g_fixture_raw_err = ESP_OK;
    g_geometry_read_calls = 0U;
    g_localization_geometry_calls = 0U;
    g_summary_read_calls = 0U;
    g_range_read_calls = 0U;
    g_range_read_max_frames = 0U;
    g_fixture_gap_index = SIZE_MAX;
    g_fixture_nonmonotonic_index = SIZE_MAX;
    memset(g_fixture_rail_counts, 0, sizeof(g_fixture_rail_counts));

    g_recorded_count = 0U;
    g_write_call_count = 0U;
    g_pick_write_calls = 0U;
    memset(&g_pick_record, 0, sizeof(g_pick_record));
    g_fail_write_at_set = false;
    g_fail_write_at_index = 0U;
    g_fail_write_err = ESP_ERR_NO_MEM;
}


static const localization_run_record_t *find_record(const char *method_name)
{
    for (size_t i = 0U; i < g_recorded_count; ++i) {
        if (strcmp(g_recorded[i].method_name, method_name) == 0) {
            return &g_recorded[i];
        }
    }
    return NULL;
}


/* ==== Synthetic event generation ==========================================
 * Same deterministic xorshift32 broadband-noise approach as
 * tests/host/localization_pipeline_validation_test.c (MAX-141), reused here
 * because it is already proven not to alias against a short STA window. */

static uint32_t s_noise_state;

static float next_noise_sample(float amplitude)
{
    s_noise_state ^= s_noise_state << 13;
    s_noise_state ^= s_noise_state >> 17;
    s_noise_state ^= s_noise_state << 5;
    const float unit = (float)(s_noise_state % 20001u) / 10000.0f - 1.0f;
    return amplitude * unit;
}

/* A small tetrahedron: MAIN-relative REMOTE positions, meters. */
static const localization_point3d_t s_pod_positions[OBJECT_STORE_POD_COUNT] = {
    { 0.0, 0.0, 0.0 },
    { 2.0, 0.0, 0.0 },
    { 0.0, 2.0, 0.0 },
    { 0.0, 0.0, 2.0 }
};

#define FIX_SAMPLE_RATE_HZ  50000U
#define FIX_STA_W           10U
#define FIX_LTA_W           100U
#define FIX_NOISE_W         50U
#define FIX_PULSE_W         20U
#define FIX_REFINE          10U

static void free_fixture_frames(void)
{
    free(g_fixture_frames);
    g_fixture_frames = NULL;
    g_fixture_frame_count = 0U;
}

/*
 * Builds a session fixture with `pod_count` participating REMOTEs (the
 * first pod_count entries of s_pod_positions) and, when include_pulse,
 * a clean synthetic arrival from `source` at `speed_mps` embedded on every
 * participating channel -- exact ground truth, so a solved position can be
 * checked against it.
 */
static void build_fixture(
    size_t pod_count,
    const localization_point3d_t *source,
    double speed_mps,
    double emission_s,
    float pulse_amplitude,
    float noise_amplitude,
    bool include_pulse
)
{
    memset(&g_fixture_geometry, 0, sizeof(g_fixture_geometry));

    size_t arrival_index[OBJECT_STORE_POD_COUNT] = { 0 };
    size_t buffer_len = 0U;

    for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
        if (ch >= pod_count) {
            continue;
        }
        g_fixture_geometry.pods[ch].computed = true;
        g_fixture_geometry.pods[ch].leveled = true;
        g_fixture_geometry.pods[ch].x_mm = (int32_t)llround(s_pod_positions[ch].x_m * 1000.0);
        g_fixture_geometry.pods[ch].y_mm = (int32_t)llround(s_pod_positions[ch].y_m * 1000.0);
        g_fixture_geometry.pods[ch].z_mm = (int32_t)llround(s_pod_positions[ch].z_m * 1000.0);
        g_fixture_geometry.pods[ch].computed_at_ms = 1000ULL;

        if (include_pulse) {
            const double dx = source->x_m - s_pod_positions[ch].x_m;
            const double dy = source->y_m - s_pod_positions[ch].y_m;
            const double dz = source->z_m - s_pod_positions[ch].z_m;
            const double distance = sqrt(dx * dx + dy * dy + dz * dz);
            const double arrival_s = emission_s + distance / speed_mps;
            arrival_index[ch] = (size_t)llround(arrival_s * (double)FIX_SAMPLE_RATE_HZ);
            const size_t needed = arrival_index[ch] + FIX_PULSE_W + FIX_NOISE_W + 100U;
            if (needed > buffer_len) {
                buffer_len = needed;
            }
        }
    }
    if (buffer_len == 0U) {
        buffer_len = 800U;
    }

    free_fixture_frames();
    g_fixture_frames = calloc(buffer_len, sizeof(*g_fixture_frames));
    assert(g_fixture_frames != NULL);
    g_fixture_frame_count = buffer_len;

    for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
        s_noise_state = 0x9E3779B9u ^ ((uint32_t)ch * 0x85EBCA6Bu + 1u);
        for (size_t i = 0U; i < buffer_len; ++i) {
            g_fixture_frames[i].channel_uv[ch] = next_noise_sample(noise_amplitude);
        }
        if (include_pulse && ch < pod_count) {
            size_t end = arrival_index[ch] + FIX_PULSE_W;
            if (end > buffer_len) {
                end = buffer_len;
            }
            for (size_t i = arrival_index[ch]; i < end; ++i) {
                g_fixture_frames[i].channel_uv[ch] = pulse_amplitude;
            }
        }
    }

    memset(&g_fixture_summary, 0, sizeof(g_fixture_summary));
    for (size_t i = 0U; i < buffer_len; ++i) {
        g_fixture_frames[i].timestamp_us = 5000000LL + (int64_t)i * 20LL;
    }
    g_fixture_summary.sample_rate_hz = FIX_SAMPLE_RATE_HZ;
    g_fixture_summary.channel_count = OBJECT_STORE_POD_COUNT;
    g_fixture_summary.samples_written = buffer_len;
    g_fixture_summary.complete = true;
    g_fixture_summary.readable = true;
}


static void extend_fixture_frames(size_t frame_count)
{
    assert(frame_count >= g_fixture_frame_count);
    seismic_frame_t *extended = realloc(
        g_fixture_frames,
        sizeof(*g_fixture_frames) * frame_count
    );
    assert(extended != NULL);
    memset(
        extended + g_fixture_frame_count,
        0,
        sizeof(*g_fixture_frames) * (frame_count - g_fixture_frame_count)
    );
    for (size_t i = g_fixture_frame_count; i < frame_count; ++i) {
        extended[i].timestamp_us = 5000000LL + (int64_t)i * 20LL;
    }
    g_fixture_frames = extended;
    g_fixture_frame_count = frame_count;
    g_fixture_summary.samples_written = frame_count;
}


static localization_calculate_config_t default_test_config(void)
{
    localization_calculate_config_t config;
    memset(&config, 0, sizeof(config));
    config.propagation_speed_mps = 340.0;
    config.propagation_speed_uncertainty_mps = 20.0;
    snprintf(config.propagation_model_name, sizeof(config.propagation_model_name), "%s", "test-model");
    config.timing_uncertainty_us = 60.0;
    config.sample_rate_hz = FIX_SAMPLE_RATE_HZ;
    config.pick_config.noise_window_samples = FIX_NOISE_W;
    config.pick_config.sta_window_samples = FIX_STA_W;
    config.pick_config.lta_window_samples = FIX_LTA_W;
    config.pick_config.sta_lta_trigger_ratio = 3.0f;
    config.pick_config.refine_lookback_samples = FIX_REFINE;
    config.pick_config.refine_lookahead_samples = FIX_REFINE;
    config.pick_config.refine_amplitude_fraction = 0.5f;
    config.pick_config.snr_signal_window_samples = FIX_PULSE_W;
    config.pick_config.minimum_snr = 3.0f;
    config.semblance_window_half_span_samples = 10U;
    config.max_source_depth_m = 0.0;
    config.s_pick_enabled = false;
    config.deadline_us = 0;
    config.progress = NULL;
    config.progress_context = NULL;
    return config;
}


static void test_non_finite_velocity_is_rejected_before_io(void)
{
    localization_calculate_config_t config = default_test_config();
    localization_calculate_summary_t summary;
    config.propagation_speed_mps = INFINITY;
    g_geometry_read_calls = 0U;
    g_localization_geometry_calls = 0U;
    assert(localization_calculate_run("OBJ_0001", "S000001", &config, 1U, &summary) == ESP_ERR_INVALID_ARG);
    assert(g_geometry_read_calls == 0U);

    config = default_test_config();
    config.propagation_speed_mps = NAN;
    assert(localization_calculate_run("OBJ_0001", "S000001", &config, 1U, &summary) == ESP_ERR_INVALID_ARG);
    assert(g_geometry_read_calls == 0U);

    config = default_test_config();
    config.propagation_speed_uncertainty_mps = NAN;
    assert(localization_calculate_run("OBJ_0001", "S000001", &config, 1U, &summary) == ESP_ERR_INVALID_ARG);
    assert(g_geometry_read_calls == 0U);

    config = default_test_config();
    config.velocity_range.bounds = (localization_velocity_bounds_t) {
        .v_min_mps = 100.0f,
        .v_max_mps = INFINITY,
        .vp_vs_min = 1.5f,
        .vp_vs_max = 2.0f,
        .set = true,
    };
    assert(localization_calculate_run("OBJ_0001", "S000001", &config, 1U, &summary) == ESP_ERR_INVALID_ARG);
    assert(g_geometry_read_calls == 0U);
}


static localization_calculate_config_t rate_fixture_config(uint32_t settings_rate_hz)
{
    localization_calculate_config_t config = default_test_config();
    config.sample_rate_hz = settings_rate_hz;
    config.pick_config.noise_window_samples = 200U;
    config.pick_config.sta_window_samples = 40U;
    config.pick_config.lta_window_samples = 400U;
    config.pick_config.refine_lookback_samples = 40U;
    config.pick_config.refine_lookahead_samples = 40U;
    config.pick_config.snr_signal_window_samples = 80U;
    config.semblance_window_half_span_samples = 40U;
    return config;
}


static void test_sample_windows_rescale_across_session_rates(void)
{
    localization_calculate_config_t config = rate_fixture_config(20000U);
    char reason[96];
    assert(localization_calculate_rescale_sample_windows(&config, 0U, reason, sizeof(reason)) == ESP_ERR_INVALID_ARG);
    assert(strstr(reason, "session sample rate") != NULL);

    config = rate_fixture_config(20000U);
    assert(localization_calculate_rescale_sample_windows(&config, 500U, reason, sizeof(reason)) == ESP_OK);
    assert(config.sample_rate_hz == 500U);
    assert(config.pick_config.noise_window_samples == 5U);
    assert(config.pick_config.sta_window_samples == 1U);
    assert(config.pick_config.lta_window_samples == 10U);
    assert(config.pick_config.refine_lookback_samples == 1U);
    assert(config.pick_config.refine_lookahead_samples == 1U);
    assert(config.pick_config.snr_signal_window_samples == 2U);
    assert(config.semblance_window_half_span_samples == 1U);
    assert(reason[0] == '\0');

    config = rate_fixture_config(500U);
    assert(localization_calculate_rescale_sample_windows(&config, 20000U, reason, sizeof(reason)) == ESP_OK);
    assert(config.pick_config.noise_window_samples == 8000U);
    assert(config.pick_config.sta_window_samples == 1600U);
    assert(config.pick_config.lta_window_samples == 16000U);
    assert(config.pick_config.refine_lookback_samples == 1600U);
    assert(config.pick_config.refine_lookahead_samples == 1600U);
    assert(config.pick_config.snr_signal_window_samples == 3200U);
    assert(config.semblance_window_half_span_samples == 1600U);

    config = rate_fixture_config(500U);
    const localization_calculate_config_t identity = config;
    assert(localization_calculate_rescale_sample_windows(&config, 500U, reason, sizeof(reason)) == ESP_OK);
    assert(memcmp(&config.pick_config, &identity.pick_config, sizeof(config.pick_config)) == 0);
    assert(config.semblance_window_half_span_samples == identity.semblance_window_half_span_samples);

    config = rate_fixture_config(500U);
    config.pick_config.sta_window_samples = 10000000U;
    config.pick_config.lta_window_samples = 30000001U;
    config.pick_config.sta_lta_trigger_ratio = 3.0f;
    assert(localization_calculate_rescale_sample_windows(&config, 500U, reason, sizeof(reason)) == ESP_OK);

    config = rate_fixture_config(0U); /* in-memory legacy profile means 500 Hz */
    assert(localization_calculate_rescale_sample_windows(&config, 20000U, reason, sizeof(reason)) == ESP_OK);
    assert(config.sample_rate_hz == 20000U && config.pick_config.sta_window_samples == 1600U);

    config = rate_fixture_config(3U);
    config.pick_config.noise_window_samples = 4U;
    config.pick_config.sta_window_samples = 2U;
    config.pick_config.lta_window_samples = 6U;
    config.pick_config.refine_lookback_samples = 1U;
    config.pick_config.refine_lookahead_samples = 2U;
    config.pick_config.snr_signal_window_samples = 4U;
    config.semblance_window_half_span_samples = 1U;
    assert(localization_calculate_rescale_sample_windows(&config, 2U, reason, sizeof(reason)) == ESP_OK);
    assert(config.pick_config.noise_window_samples == 3U); /* 4 * 2/3 rounds to nearest */
    assert(config.pick_config.sta_window_samples == 1U); /* min 1 */
    assert(config.pick_config.lta_window_samples == 4U);
    assert(config.semblance_window_half_span_samples == 1U);

    config = rate_fixture_config(2U);
    config.pick_config.noise_window_samples = 3U;
    assert(localization_calculate_rescale_sample_windows(&config, 1U, reason, sizeof(reason)) == ESP_OK);
    assert(config.pick_config.noise_window_samples == 2U); /* exact half rounds upward */

    config = rate_fixture_config(500U);
    config.s_pick_enabled = true;
    config.s_pick_config.min_separation_samples = 120U;
    config.s_pick_config.max_separation_samples = 800U;
    config.s_pick_config.sta_window_samples = 30U;
    config.s_pick_config.lta_window_samples = 300U;
    config.s_pick_config.sta_lta_trigger_ratio = 4.0f;
    assert(localization_calculate_rescale_sample_windows(&config, 20000U, reason, sizeof(reason)) == ESP_OK);
    assert(config.s_pick_config.min_separation_samples == 4800U);
    assert(config.s_pick_config.max_separation_samples == 32000U);
    assert(config.s_pick_config.sta_window_samples == 1200U);
    assert(config.s_pick_config.lta_window_samples == 12000U);

    config = rate_fixture_config(20000U);
    config.s_pick_enabled = true;
    config.s_pick_config.min_separation_samples = 120U;
    config.s_pick_config.max_separation_samples = 800U;
    config.s_pick_config.sta_window_samples = 30U;
    config.s_pick_config.lta_window_samples = 300U;
    config.s_pick_config.sta_lta_trigger_ratio = 4.0f;
    assert(localization_calculate_rescale_sample_windows(&config, 500U, reason, sizeof(reason)) == ESP_OK);
    assert(config.s_pick_config.min_separation_samples == 3U);
    assert(config.s_pick_config.max_separation_samples == 20U);
    assert(config.s_pick_config.sta_window_samples == 1U);
    assert(config.s_pick_config.lta_window_samples == 8U);

    config = rate_fixture_config(2U);
    config.s_pick_enabled = true;
    config.s_pick_config.min_separation_samples = 1U;
    config.s_pick_config.max_separation_samples = 2U;
    config.s_pick_config.sta_window_samples = 2U;
    config.s_pick_config.lta_window_samples = 4U;
    config.s_pick_config.sta_lta_trigger_ratio = 1.0f;
    assert(localization_calculate_rescale_sample_windows(&config, 1U, reason, sizeof(reason)) == ESP_ERR_INVALID_ARG);
    assert(strstr(reason, "S-wave") != NULL);

    config = rate_fixture_config(2U);
    config.pick_config.sta_window_samples = 1U;
    config.pick_config.lta_window_samples = 2U;
    config.pick_config.sta_lta_trigger_ratio = 0.5f;
    assert(localization_calculate_rescale_sample_windows(&config, 1U, reason, sizeof(reason)) == ESP_ERR_INVALID_ARG);
    assert(strstr(reason, "STA/LTA") != NULL);

    config = rate_fixture_config(2U);
    config.pick_config.sta_window_samples = 3U;
    config.pick_config.lta_window_samples = 5U;
    config.pick_config.sta_lta_trigger_ratio = 1.6f;
    assert(localization_calculate_rescale_sample_windows(&config, 1U, reason, sizeof(reason)) == ESP_ERR_INVALID_ARG);
    assert(strstr(reason, "trigger ratio") != NULL);
}


static void test_run_revalidates_windows_against_session_rate(void)
{
    const localization_point3d_t source = { 0.25, 0.40, 0.30 };
    reset_fakes();
    build_fixture(4U, &source, 340.0, 0.01, 1.0f, 0.02f, true);
    g_fixture_summary.sample_rate_hz = 500U;

    localization_calculate_config_t config = default_test_config();
    config.sample_rate_hz = 20000U;
    config.pick_config.noise_window_samples = 1U;
    config.pick_config.sta_window_samples = 1U;
    config.pick_config.lta_window_samples = 2U;
    config.pick_config.sta_lta_trigger_ratio = 1.5f;
    config.pick_config.refine_lookback_samples = 1U;
    config.pick_config.refine_lookahead_samples = 1U;
    config.pick_config.snr_signal_window_samples = 1U;
    config.semblance_window_half_span_samples = 1U;

    localization_calculate_summary_t summary;
    const esp_err_t err = localization_calculate_run(
        "OBJ01", "S000099", &config, 46000ULL, &summary
    );
    assert(err == ESP_ERR_INVALID_ARG); /* 1/2 samples collapse to 1/1 at 500 Hz */
    assert(
        g_geometry_read_calls == 1U && g_localization_geometry_calls == 1U && g_summary_read_calls == 1U &&
        g_range_read_calls == 0U
    );
    assert(g_write_call_count == 0U && g_pick_write_calls == 0U);
    free_fixture_frames();
}


static const localization_point3d_t s_source = { 0.25, 0.40, 0.30 };
#define TEST_SPEED_MPS 340.0
#define TEST_EMISSION_S 0.01


static size_t fixture_p_arrival_index(
    size_t channel,
    const localization_point3d_t *source,
    double speed_mps,
    double emission_s
)
{
    const double dx = source->x_m - s_pod_positions[channel].x_m;
    const double dy = source->y_m - s_pod_positions[channel].y_m;
    const double dz = source->z_m - s_pod_positions[channel].z_m;
    return (size_t)llround((emission_s + sqrt(dx * dx + dy * dy + dz * dz) / speed_mps) *
                           (double)FIX_SAMPLE_RATE_HZ);
}


static void add_ramp_pulse_to_channel(size_t channel, size_t onset, size_t width, float amplitude)
{
    for (size_t k = 0U; k < width && onset + k < g_fixture_frame_count; ++k) {
        const float envelope = k < 8U ? (float)(k + 1U) / 8.0f : 1.0f;
        g_fixture_frames[onset + k].channel_uv[channel] += amplitude * envelope;
    }
}


/* ==== Scenarios ============================================================ */

/*
 * MAX-168 dimensionality policy: fewer than 3 participating REMOTEs must
 * fail closed with ESP_ERR_INVALID_STATE and write NOTHING to the run
 * store -- not even a partial/failed record.
 */
static void test_insufficient_dimensionality_writes_nothing(void)
{
    for (size_t pod_count = 0U; pod_count <= 2U; ++pod_count) {
        reset_fakes();
        build_fixture(pod_count, &s_source, TEST_SPEED_MPS, TEST_EMISSION_S, 1.0f, 0.02f, true);

        localization_calculate_config_t config = default_test_config();
        localization_calculate_summary_t summary;
        const esp_err_t err = localization_calculate_run(
            "OBJ01", "S000001", &config, 42000ULL, &summary
        );

        assert(err == ESP_ERR_INVALID_STATE);
        assert(summary.participating_channel_count == pod_count);
        assert(g_geometry_read_calls == 1U);
        assert(g_summary_read_calls == 0U && g_range_read_calls == 0U);
        assert(g_write_call_count == 0U);
        assert(g_pick_write_calls == 0U);
        assert(g_recorded_count == 0U);
    }
    free_fixture_frames();
    puts("  insufficient dimensionality (0/1/2 pods): zero run-store writes -- OK");
}


static void test_truncated_session_persists_analysis_window(void)
{
    reset_fakes();
    build_fixture(4U, &s_source, TEST_SPEED_MPS, TEST_EMISSION_S, 1.0f, 0.02f, true);
    const size_t bytes_per_frame = sizeof(seismic_frame_t) +
        SEISMIC_CHANNEL_COUNT * sizeof(float) + sizeof(uint64_t) + sizeof(bool);
    const size_t expected_window_frames =
        (3072U * 1024U) / bytes_per_frame;
    extend_fixture_frames(expected_window_frames + 1U);

    localization_calculate_config_t config = default_test_config();
    localization_calculate_summary_t summary;
    assert(localization_calculate_run(
        "OBJ01", "S000004", &config, 45000ULL, &summary
    ) == ESP_OK);
    assert(g_summary_read_calls == 1U && g_range_read_calls == 1U);
    assert(g_recorded_count > 0U);
    for (size_t i = 0U; i < g_recorded_count; ++i) {
        if (!g_recorded[i].analysis_window_truncated) {
            fprintf(stderr, "missing window provenance record=%zu method=%s start=%llu frames=%llu samples=%llu\n",
                i, g_recorded[i].method_name,
                (unsigned long long)g_recorded[i].analysis_window_start_index,
                (unsigned long long)g_recorded[i].analysis_window_frames,
                (unsigned long long)g_fixture_summary.samples_written);
        }
        assert(g_recorded[i].analysis_window_truncated);
        assert(g_recorded[i].analysis_window_start_index == 0U);
        assert(g_recorded[i].analysis_window_frames == expected_window_frames);
    }
    free_fixture_frames();
}


static void test_limited_free_psram_truncates_with_memory_provenance(void)
{
    reset_fakes();
    localization_test_psram_model_set_reserved(8U * 1024U * 1024U - 1536U * 1024U);
    build_fixture(4U, &s_source, TEST_SPEED_MPS, TEST_EMISSION_S, 1.0f, 0.02f, true);
    extend_fixture_frames(39322U);
    g_fixture_summary.sample_rate_hz = 20000U;
    const size_t bytes_per_frame = sizeof(seismic_frame_t) +
        SEISMIC_CHANNEL_COUNT * sizeof(float) + sizeof(uint64_t) + sizeof(bool);
    /* 1.5 MiB free, minus the 0.5 MiB reserve of a session without a scan, leaves a 1 MiB window. */
    const size_t expected_window_frames = (1024U * 1024U) / bytes_per_frame;

    localization_calculate_config_t config = default_test_config();
    localization_calculate_summary_t summary;
    assert(localization_calculate_run(
        "OBJ01", "S000007", &config, 48000ULL, &summary
    ) == ESP_OK);
    assert(g_recorded_count > 0U);
    for (size_t i = 0U; i < g_recorded_count; ++i) {
        assert(g_recorded[i].analysis_window_truncated);
        assert(g_recorded[i].analysis_window_frames == expected_window_frames);
        assert(strcmp(g_recorded[i].analysis_window_reason, "memory") == 0);
    }
    assert(localization_test_psram_model_allocated_bytes() == 0U);
    free_fixture_frames();
    localization_test_psram_model_reset();
}


static void test_insufficient_free_psram_records_internal_failure(void)
{
    reset_fakes();
    localization_test_psram_model_set_reserved(8U * 1024U * 1024U - 600U * 1024U);
    build_fixture(4U, &s_source, TEST_SPEED_MPS, TEST_EMISSION_S, 1.0f, 0.02f, true);
    g_fixture_summary.sample_rate_hz = 20000U;

    localization_calculate_config_t config = default_test_config();
    localization_calculate_summary_t summary;
    assert(localization_calculate_run(
        "OBJ01", "S000008", &config, 49000ULL, &summary
    ) == ESP_ERR_NO_MEM);
    assert(g_recorded_count == 1U);
    assert(strcmp(g_recorded[0].rejection_reason, "insufficient_memory") == 0);
    assert(g_recorded[0].failure_class == LOCALIZATION_FAILURE_CLASS_INTERNAL);
    assert(summary.methods_total == 1U && summary.methods_persisted == 1U);
    assert(localization_test_psram_model_allocated_bytes() == 0U);
    free_fixture_frames();
    localization_test_psram_model_reset();
}


static void test_record_stop_then_localize_with_reserved_psram(void)
{
    reset_fakes();
    free_fixture_frames();
    localization_test_psram_model_set_reserved(8U * 1024U * 1024U - 4U * 1024U * 1024U);
    assert(localization_test_psram_model_free_bytes() == 4U * 1024U * 1024U);
    assert(seismic_buffer_init() == ESP_OK);
    assert(seismic_buffer_configure(SEISMIC_SOURCE_AD7606, 20000U) == ESP_OK);
    assert(seismic_buffer_capacity() == 40000U);

    const float samples[SEISMIC_CHANNEL_COUNT] = {1.0f, -1.0f, 2.0f, -2.0f};
    for (uint64_t i = 0U; i < 20000U; ++i) {
        assert(seismic_buffer_push_indexed((int64_t)i * 50, i, samples) == ESP_OK);
    }
    assert(localization_test_psram_model_free_bytes() < 4U * 1024U * 1024U);
    assert(seismic_buffer_configure(SEISMIC_SOURCE_NONE, 0U) == ESP_OK);
    assert(seismic_buffer_capacity() == 0U);
    assert(localization_test_psram_model_free_bytes() == 4U * 1024U * 1024U);

    build_fixture(4U, &s_source, TEST_SPEED_MPS, TEST_EMISSION_S, 0.0f, 0.0f, false);
    extend_fixture_frames(20000U);
    g_fixture_summary.sample_rate_hz = 20000U;
    localization_calculate_config_t config = default_test_config();
    localization_calculate_summary_t summary;
    assert(localization_calculate_run(
        "OBJ01", "S000009", &config, 50000ULL, &summary
    ) == ESP_OK);
    assert(g_range_read_calls == 1U);
    assert(g_recorded_count > 0U);
    assert(localization_test_psram_model_allocated_bytes() == 0U);
    assert(localization_test_psram_model_free_bytes() == 4U * 1024U * 1024U);
    free_fixture_frames();
    localization_test_psram_model_reset();
}


/*
 * 3 participating REMOTEs: every fused unconstrained method (12) plus the
 * joint fusion (1) is persisted, every solved record's dimensionality is
 * PLANAR_2D with a Z fixed at the array's own mean sensor height (never a
 * fabricated depth), and TDOA (which MAX-212 always solves in 3D) is
 * unsolved with a reason because 3 sensors cannot support a 3D solve.
 */
static void test_three_pods_planar_no_fabricated_z(void)
{
    reset_fakes();
    build_fixture(3U, &s_source, TEST_SPEED_MPS, TEST_EMISSION_S, 1.0f, 0.02f, true);

    localization_calculate_config_t config = default_test_config();
    localization_calculate_summary_t summary;
    const esp_err_t err = localization_calculate_run(
        "OBJ01", "S000002", &config, 43000ULL, &summary
    );

    assert(err == ESP_OK);
    assert(summary.participating_channel_count == 3U);
    assert(g_recorded_count == 13U); /* 12 unconstrained methods + joint fusion */
    assert(g_write_call_count == 13U);

    const double expected_mean_z = (s_pod_positions[0].z_m + s_pod_positions[1].z_m + s_pod_positions[2].z_m) / 3.0;

    bool any_solved = false;
    for (size_t i = 0U; i < g_recorded_count; ++i) {
        const localization_run_record_t *record = &g_recorded[i];
        assert(record->dimensionality == LOCALIZATION_DIMENSIONALITY_PLANAR_2D);
        if (record->solved) {
            any_solved = true;
            /* Never a fabricated depth: every planar solve sits on the
             * one fixed Z plane the search box collapsed to. */
            assert(fabs(record->source_position.z_m - expected_mean_z) < 1.0e-6);
        } else {
            assert(strlen(record->rejection_reason) > 0U);
        }
    }
    assert(any_solved);

    const localization_run_record_t *tdoa = find_record("tdoa_gauss_newton");
    assert(tdoa != NULL);
    assert(!tdoa->solved);
    assert(strlen(tdoa->rejection_reason) > 0U);

    puts("  3 pods: 13 records, all PLANAR_2D, no fabricated Z, TDOA unsolved -- OK");
}


/*
 * 4 participating REMOTEs with a clean synthetic event: CAPABLE_3D records,
 * and the TDOA (or traveltime) solution lands within a stated tolerance of
 * the true, known source.
 */
static void test_four_pods_clean_event_solves_near_truth(void)
{
    reset_fakes();
    build_fixture(4U, &s_source, TEST_SPEED_MPS, TEST_EMISSION_S, 1.0f, 0.02f, true);

    localization_calculate_config_t config = default_test_config();
    snprintf(config.material, sizeof(config.material), "concrete");
    localization_calculate_summary_t summary;
    const esp_err_t err = localization_calculate_run(
        "OBJ01", "S000003", &config, 44000ULL, &summary
    );

    assert(err == ESP_OK);
    assert(summary.participating_channel_count == 4U);
    assert(summary.methods_total == 13U && summary.methods_completed == 13U);
    assert(g_recorded_count == 13U);
    assert(g_pick_record.session_epoch_known);
    assert(g_pick_record.session_epoch_timestamp_us == 5000000LL);
    assert(!g_pick_record.channels[0].s_pick.present);

    for (size_t i = 0U; i < g_recorded_count; ++i) {
        assert(g_recorded[i].dimensionality == LOCALIZATION_DIMENSIONALITY_CAPABLE_3D);
        assert(strcmp(g_recorded[i].propagation_model_name, "test-model") == 0);
        assert(strcmp(g_recorded[i].material, "concrete") == 0);
        if (!g_recorded[i].solved) {
            localization_calculate_test_assert_rejection_class(
                &g_recorded[i], g_recorded[i].failure_class
            );
        }
    }

    const localization_run_record_t *tdoa = find_record("tdoa_gauss_newton");
    const localization_run_record_t *traveltime = find_record("traveltime_inversion");
    assert(tdoa != NULL && traveltime != NULL);

    /* At least one of the two classic solvers must have converged near the
     * known synthetic source -- stated tolerance: 0.2 m over a ~2 m array,
     * generous for a 50 kHz / xcorr-refined synthetic pick. */
    const double tolerance_m = 0.2;
    bool within_tolerance = false;
    const localization_run_record_t *solved_ref = NULL;
    if (tdoa->solved) {
        solved_ref = tdoa;
    } else if (traveltime->solved) {
        solved_ref = traveltime;
    }
    assert(solved_ref != NULL);
    assert(summary.recommendation.successful);

    const double dx = solved_ref->source_position.x_m - s_source.x_m;
    const double dy = solved_ref->source_position.y_m - s_source.y_m;
    const double dz = solved_ref->source_position.z_m - s_source.z_m;
    const double error_m = sqrt(dx * dx + dy * dy + dz * dz);
    printf(
        "  4 pods clean event: %s solved, position error = %.4f m\n",
        solved_ref->method_name, error_m
    );
    within_tolerance = error_m < tolerance_m;
    assert(within_tolerance);

    puts("  4 pods clean event: CAPABLE_3D, solved near true source -- OK");
}


static void test_gap_and_nonmonotonic_timestamps_reject_before_picking(void)
{
    static const struct {
        size_t gap_index;
        size_t nonmonotonic_index;
        const char *reason;
        const char *session_id;
    } cases[] = {
        { 25U, SIZE_MAX, LOCALIZATION_RUN_REJECTION_REASON_SESSION_GAP, "S132101" },
        { SIZE_MAX, 25U, LOCALIZATION_RUN_REJECTION_REASON_TIMESTAMP_NON_MONOTONIC, "S132102" },
    };
    for (size_t case_index = 0U; case_index < sizeof(cases) / sizeof(cases[0]); ++case_index) {
        reset_fakes();
        build_fixture(4U, &s_source, TEST_SPEED_MPS, TEST_EMISSION_S, 1.0f, 0.02f, true);
        g_fixture_gap_index = cases[case_index].gap_index;
        g_fixture_nonmonotonic_index = cases[case_index].nonmonotonic_index;
        localization_calculate_config_t config = default_test_config();
        localization_calculate_summary_t summary;
        assert(localization_calculate_run(
            "OBJ01", cases[case_index].session_id, &config, 532101U + case_index, &summary
        ) == ESP_OK);
        assert(g_recorded_count == 1U && g_pick_write_calls == 0U);
        assert(strcmp(g_recorded[0].method_name, "localization_analysis") == 0);
        assert(strcmp(g_recorded[0].rejection_reason, cases[case_index].reason) == 0);
        assert(g_recorded[0].session_epoch_known);
        assert(g_recorded[0].session_epoch_timestamp_us == 5000000LL);
        assert(summary.methods_total == 1U && summary.methods_persisted == 1U);
        free_fixture_frames();
    }
    puts("  gap and non-monotonic timestamps persist exact rejection reason and sample-zero epoch before picks");
}


static void test_duplicate_sensor_position_persists_validation_reason(void)
{
    reset_fakes();
    build_fixture(4U, &s_source, TEST_SPEED_MPS, TEST_EMISSION_S, 1.0f, 0.02f, true);
    g_fixture_geometry.pods[3].x_mm = g_fixture_geometry.pods[0].x_mm;
    g_fixture_geometry.pods[3].y_mm = g_fixture_geometry.pods[0].y_mm;
    g_fixture_geometry.pods[3].z_mm = g_fixture_geometry.pods[0].z_mm;
    localization_calculate_config_t config = default_test_config();
    localization_calculate_summary_t summary;
    assert(localization_calculate_run("OBJ01", "S132103", &config, 532103U, &summary) == ESP_OK);
    const localization_run_record_t *tdoa = find_record("tdoa_gauss_newton");
    assert(tdoa != NULL && !tdoa->solved);
    assert(strcmp(tdoa->rejection_reason,
                  LOCALIZATION_RUN_REJECTION_REASON_DUPLICATE_SENSOR_POSITION) == 0);
    assert(tdoa->session_epoch_known && tdoa->session_epoch_timestamp_us == 5000000LL);
    puts("  duplicate sensor positions persist duplicate_sensor_position on the TDOA record");
    free_fixture_frames();
}


static void test_saturated_and_ambiguous_picks_are_excluded(void)
{
    reset_fakes();
    build_fixture(4U, &s_source, TEST_SPEED_MPS, TEST_EMISSION_S, 1.0f, 0.02f, true);
    g_fixture_rail_counts[0] = 8U;
    localization_calculate_config_t config = default_test_config();
    localization_calculate_summary_t summary;
    assert(localization_calculate_run("OBJ01", "S132104", &config, 532104U, &summary) == ESP_OK);
    assert(g_pick_record.channels[0].pick.status == GEOPHONE_PICK_REJECTED_SATURATED);
    assert(strcmp(g_pick_record.channels[0].reason, "saturated") == 0);
    for (size_t i = 0U; i < g_recorded_count; ++i) {
        assert((g_recorded[i].channel_mask_used & 1U) == 0U);
    }
    free_fixture_frames();

    reset_fakes();
    build_fixture(4U, &s_source, TEST_SPEED_MPS, TEST_EMISSION_S, 1.0f, 0.02f, true);
    const size_t p_index = fixture_p_arrival_index(0U, &s_source, TEST_SPEED_MPS, TEST_EMISSION_S);
    extend_fixture_frames(p_index + 1200U);
    s_noise_state = 0x13201313U;
    for (size_t i = 0U; i < g_fixture_frame_count; ++i) {
        g_fixture_frames[i].channel_uv[0] = next_noise_sample(0.02f);
    }
    add_ramp_pulse_to_channel(0U, p_index, 32U, 0.45f);
    add_ramp_pulse_to_channel(0U, p_index + 800U, 32U, 1.2f);
    assert(localization_calculate_run("OBJ01", "S132105", &config, 532105U, &summary) == ESP_OK);
    assert(g_pick_record.channels[0].pick.status == GEOPHONE_PICK_REJECTED_AMBIGUOUS);
    assert(strcmp(g_pick_record.channels[0].reason, "ambiguous") == 0);
    for (size_t i = 0U; i < g_recorded_count; ++i) {
        assert((g_recorded[i].channel_mask_used & 1U) == 0U);
    }
    puts("  saturated and two-event ambiguous channels retain reasons and never enter solver masks");
    free_fixture_frames();
}


static void test_secondary_pick_evidence_is_persisted_only_when_enabled(void)
{
    static const struct {
        float minimum_snr;
        geophone_pick_status_t expected_status;
        const char *expected_reason;
        const char *session_id;
    } cases[] = {
        { 2.0f, GEOPHONE_PICK_ACCEPTED, "accepted", "S132106" },
        { 2000000.0f, GEOPHONE_PICK_REJECTED_LOW_SNR, "low_snr", "S132107" },
    };
    for (size_t case_index = 0U; case_index < sizeof(cases) / sizeof(cases[0]); ++case_index) {
        reset_fakes();
        build_fixture(4U, &s_source, TEST_SPEED_MPS, TEST_EMISSION_S, 1.0f, 0.02f, true);
        size_t last_sample = 0U;
        for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
            const size_t p_index = fixture_p_arrival_index(ch, &s_source, TEST_SPEED_MPS, TEST_EMISSION_S);
            if (p_index + 950U > last_sample) last_sample = p_index + 950U;
        }
        extend_fixture_frames(last_sample);
        for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
            const size_t p_index = fixture_p_arrival_index(ch, &s_source, TEST_SPEED_MPS, TEST_EMISSION_S);
            add_ramp_pulse_to_channel(ch, p_index + 500U, 40U, 3.0f);
        }

        localization_calculate_config_t config = default_test_config();
        /* Keep the known P-to-S separation outside the P picker's second-
         * crossing ambiguity guard; the independent S picker still sees it. */
        config.pick_config.lta_window_samples = 600U;
        config.s_pick_enabled = true;
        config.s_pick_config = (geophone_secondary_pick_config_t){
            .min_separation_samples = 321U,
            .max_separation_samples = 900U,
            .sta_window_samples = 10U,
            .lta_window_samples = 60U,
            .sta_lta_trigger_ratio = 3.0f,
            .minimum_snr = cases[case_index].minimum_snr,
        };
        localization_calculate_summary_t summary;
        assert(localization_calculate_run(
            "OBJ01", cases[case_index].session_id, &config, 532106U + case_index, &summary
        ) == ESP_OK);
        assert(g_pick_record.session_epoch_known && g_pick_record.session_epoch_timestamp_us == 5000000LL);
        assert(find_record("psafuvs") != NULL && find_record("psp_joint") != NULL);
        bool saw_s_pick = false;
        for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
            const localization_pick_phase_t *phase = &g_pick_record.channels[ch].s_pick;
            if (g_pick_record.channels[ch].pick.status != GEOPHONE_PICK_ACCEPTED) {
                assert(!phase->present);
                continue;
            }
            saw_s_pick = true;
            assert(phase->present);
            assert(phase->status == cases[case_index].expected_status);
            assert(strcmp(phase->reason, cases[case_index].expected_reason) == 0);
            assert((phase->fields_present & LOCALIZATION_PICK_PHASE_FIELD_STATUS) != 0U);
            assert((phase->fields_present & LOCALIZATION_PICK_PHASE_FIELD_METHOD) != 0U);
            if (cases[case_index].expected_status == GEOPHONE_PICK_ACCEPTED) {
                assert(phase->fields_present == LOCALIZATION_PICK_PHASE_FIELDS_ALL);
                assert((int64_t)phase->arrival_time_us > 0);
            } else {
                assert((phase->fields_present & LOCALIZATION_PICK_PHASE_FIELD_SAMPLE_INDEX) != 0U);
                assert((phase->fields_present & LOCALIZATION_PICK_PHASE_FIELD_SNR) != 0U);
            }
        }
        assert(saw_s_pick);
        free_fixture_frames();
    }
    puts("  accepted and rejected S-pick phases persist for PSAFUVS/PSP; non-S runs leave them absent");
}


/*
 * Every channel rejected (flat signal, no injected arrival): every method
 * is persisted as unsolved with a non-empty reason, and nothing crashes.
 */
static void test_flat_signal_every_method_unsolved_with_reason(void)
{
    reset_fakes();
    build_fixture(4U, &s_source, TEST_SPEED_MPS, TEST_EMISSION_S, 0.0f, 0.01f, false);

    localization_calculate_config_t config = default_test_config();
    localization_calculate_summary_t summary;
    const esp_err_t err = localization_calculate_run(
        "OBJ01", "S000004", &config, 45000ULL, &summary
    );

    assert(err == ESP_OK);
    assert(g_recorded_count == 13U);
    assert(summary.methods_solved == 0U);
    assert(summary.methods_total == 13U && summary.methods_completed == 13U);
    assert(!summary.recommendation.successful);
    assert(summary.recommendation.action == LOCALIZATION_RECOMMENDATION_RECORD_AGAIN);

    for (size_t i = 0U; i < g_recorded_count; ++i) {
        assert(!g_recorded[i].solved);
        assert(strlen(g_recorded[i].rejection_reason) > 0U);
        assert(g_recorded[i].failure_class != LOCALIZATION_FAILURE_CLASS_NONE);
        localization_calculate_test_assert_rejection_class(
            &g_recorded[i], g_recorded[i].failure_class
        );
    }

    puts("  flat signal: every method unsolved with a reason, no crash -- OK");
}


/*
 * A raw-sample read failure and a geometry read failure both propagate the
 * underlying error and cause zero run-store writes -- fail closed, no
 * partial persistence.
 */
static void test_read_failures_propagate_with_no_partial_writes(void)
{
    reset_fakes();
    build_fixture(4U, &s_source, TEST_SPEED_MPS, TEST_EMISSION_S, 1.0f, 0.02f, true);
    g_fixture_raw_err = ESP_ERR_TIMEOUT;

    localization_calculate_config_t config = default_test_config();
    localization_calculate_summary_t summary;
    esp_err_t err = localization_calculate_run("OBJ01", "S000005", &config, 46000ULL, &summary);

    assert(err == ESP_ERR_TIMEOUT);
    assert(g_geometry_read_calls == 1U);
    assert(g_summary_read_calls == 1U && g_range_read_calls == 0U);
    assert(g_write_call_count == 0U);
    assert(g_pick_write_calls == 0U);
    assert(g_recorded_count == 0U);

    reset_fakes();
    build_fixture(4U, &s_source, TEST_SPEED_MPS, TEST_EMISSION_S, 1.0f, 0.02f, true);
    g_fixture_geometry_err = ESP_FAIL;

    err = localization_calculate_run("OBJ01", "S000006", &config, 47000ULL, &summary);

    assert(err == ESP_FAIL);
    assert(g_geometry_read_calls == 1U);
    assert(g_summary_read_calls == 0U && g_range_read_calls == 0U);
    assert(g_write_call_count == 0U);
    assert(g_pick_write_calls == 0U);
    assert(g_recorded_count == 0U);

    puts("  raw-read and geometry-read failures propagate, zero writes -- OK");
}


/*
 * A run-store write failure partway through: the documented contract (see
 * localization_calculate.c's persistence loop) is "attempt every record's
 * write regardless of an earlier failure, then return the FIRST error
 * encountered" -- never abort early, never claim a false partial success.
 */
static void test_run_store_write_failure_midway_contract(void)
{
    reset_fakes();
    build_fixture(4U, &s_source, TEST_SPEED_MPS, TEST_EMISSION_S, 1.0f, 0.02f, true);

    g_fail_write_at_set = true;
    g_fail_write_at_index = 5U; /* the 6th write call (0-based) starts failing */
    g_fail_write_err = ESP_ERR_NO_MEM;

    localization_calculate_config_t config = default_test_config();
    localization_calculate_summary_t summary;
    const esp_err_t err = localization_calculate_run(
        "OBJ01", "S000007", &config, 48000ULL, &summary
    );

    /* Every one of the 13 records was still attempted -- the loop never
     * stopped early because one write failed. */
    assert(g_write_call_count == 13U);
    assert(g_recorded_count == 13U);

    /* First error wins. */
    assert(err == ESP_ERR_NO_MEM);

    /* Calls 0..4 (5 of them) succeeded; 5..12 (8 of them) failed. */
    assert(summary.methods_persisted == 5U);

    puts("  run-store write failure midway: all writes attempted, first error returned -- OK");
}


/*
 * Time budget exhausted before the run starts: every skippable method
 * (everything except tdoa_gauss_newton and energy_baseline, which always
 * run) is recorded unsolved with the exact budget reason, and the summary
 * counts them as skipped for time.
 */
static void test_time_budget_exhausted_skips_remaining_methods(void)
{
    static const char *const skippable_methods[] = {
        "traveltime_inversion", "semblance", "tdcas", "psafuvs", "suea",
        "xcorr_tdoa", "robust_tdoa", "psp_joint", "bayesian", "eikonal_fmm"
    };

    reset_fakes();
    build_fixture(4U, &s_source, TEST_SPEED_MPS, TEST_EMISSION_S, 1.0f, 0.02f, true);

    g_now_us = 2000000;

    localization_calculate_config_t config = default_test_config();
    config.deadline_us = 1000000; /* already in the past relative to g_now_us */

    localization_calculate_summary_t summary;
    const esp_err_t err = localization_calculate_run(
        "OBJ01", "S000008", &config, 49000ULL, &summary
    );

    assert(err == ESP_OK);
    assert(summary.methods_skipped_for_time == 10U);

    for (size_t i = 0U; i < sizeof(skippable_methods) / sizeof(skippable_methods[0]); ++i) {
        const localization_run_record_t *record = find_record(skippable_methods[i]);
        assert(record != NULL);
        assert(!record->solved);
        assert(strcmp(record->rejection_reason, LOCALIZATION_CALCULATE_TIME_BUDGET_REASON) == 0);
        assert(record->failure_class == LOCALIZATION_FAILURE_CLASS_INTERNAL);
        localization_calculate_test_assert_rejection_class(record, LOCALIZATION_FAILURE_CLASS_INTERNAL);
    }

    const localization_run_record_t *tdoa = find_record("tdoa_gauss_newton");
    const localization_run_record_t *energy = find_record("energy_baseline");
    assert(tdoa != NULL && energy != NULL);
    assert(summary.methods_total == 13U && summary.methods_completed == 13U);
    assert(summary.methods_skipped_for_time == 10U);
    assert(strcmp(tdoa->rejection_reason, LOCALIZATION_CALCULATE_TIME_BUDGET_REASON) != 0);
    assert(strcmp(energy->rejection_reason, LOCALIZATION_CALCULATE_TIME_BUDGET_REASON) != 0
           || energy->solved);

    puts("  time budget exhausted: 10 skippable methods report the budget reason -- OK");
}


static void prepare_recording_with_event(uint32_t duration_s, double event_time_s)
{
    const uint32_t sample_rate_hz = 500U;
    const size_t recording_frames = (size_t)duration_s * sample_rate_hz;

    build_fixture(4U, &s_source, TEST_SPEED_MPS, TEST_EMISSION_S, 0.0f, 0.0f, false);
    extend_fixture_frames(recording_frames);
    g_fixture_summary.sample_rate_hz = sample_rate_hz;

    for (size_t i = 0U; i < recording_frames; ++i) {
        g_fixture_frames[i].timestamp_us = 5000000LL + (int64_t)i * 2000LL;
        for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
            g_fixture_frames[i].channel_uv[ch] = next_noise_sample(0.02f);
        }
    }
    for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
        const double dx = s_source.x_m - s_pod_positions[ch].x_m;
        const double dy = s_source.y_m - s_pod_positions[ch].y_m;
        const double dz = s_source.z_m - s_pod_positions[ch].z_m;
        const double arrival_s = event_time_s +
            sqrt(dx * dx + dy * dy + dz * dz) / TEST_SPEED_MPS;
        const size_t arrival = (size_t)llround(arrival_s * (double)sample_rate_hz);
        add_ramp_pulse_to_channel(ch, arrival, 20U, 1.0f);
    }
}

static void run_late_event_case(uint32_t duration_s, double event_time_s, bool constrain_psram)
{
    const uint32_t sample_rate_hz = 500U;
    const size_t recording_frames = (size_t)duration_s * sample_rate_hz;
    reset_fakes();
    prepare_recording_with_event(duration_s, event_time_s);
    if (constrain_psram) {
        localization_test_psram_model_set_reserved(8U * 1024U * 1024U - 1536U * 1024U);
    }
    localization_calculate_config_t config = default_test_config();
    config.sample_rate_hz = sample_rate_hz;
    config.pick_config.noise_window_samples = 25U;
    config.pick_config.sta_window_samples = 5U;
    config.pick_config.lta_window_samples = 50U;
    config.pick_config.refine_lookback_samples = 5U;
    config.pick_config.refine_lookahead_samples = 5U;
    config.pick_config.snr_signal_window_samples = 20U;

    localization_calculate_summary_t summary;
    assert(localization_calculate_run(
        "OBJ01", "S000022", &config, 52000ULL, &summary
    ) == ESP_OK);
    assert(g_fixture_summary.samples_written == recording_frames);
    if (constrain_psram) {
        assert(g_range_read_max_frames < recording_frames);
        assert(localization_test_psram_model_allocated_bytes() == 0U);
        localization_test_psram_model_reset();
    }
    assert(summary.recommendation.action != LOCALIZATION_RECOMMENDATION_RECORD_AGAIN);
    const localization_run_record_t *tdoa = find_record("tdoa_gauss_newton");
    assert(tdoa != NULL);
    assert(tdoa->analysis_window_start_index > 0U);
    assert(tdoa->solved);
    assert(g_pick_record.session_epoch_known);
    assert(g_pick_record.session_epoch_timestamp_us == 5000000LL);
    for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
        const geophone_channel_pick_t *pick = &g_pick_record.channels[ch].pick;
        assert(pick->status == GEOPHONE_PICK_ACCEPTED);
        assert(pick->arrival_sample_index >= (size_t)(event_time_s * sample_rate_hz));
        assert(pick->arrival_sample_index < (size_t)((event_time_s + 0.1) * sample_rate_hz));
    }
    free_fixture_frames();
}

static void test_late_event_in_full_recording_is_localized(void)
{
    run_late_event_case(30U, 22.0, false);
    puts("  late event at 22 s in a 30 s recording is localized -- OK");
}

/* R5.2: an event straddling the end of the first search window (10 s) is found once, in a window that holds it whole. */
static void test_event_across_the_search_window_boundary_is_found_once(void)
{
    run_late_event_case(30U, 9.97, false);
    puts("  event straddling the 10 s window boundary is localized once -- OK");
}


static void test_event_after_45_seconds_preserves_absolute_tdoa(void)
{
    run_late_event_case(60U, 50.0, false);
    puts("  event after 45 s preserves absolute picks and epoch -- OK");
}

static void test_quiet_prefix_does_not_false_fail_late_event(void)
{
    run_late_event_case(25U, 15.0, false);
    puts("  quiet first 10 s does not reject event at 15 s -- OK");
}

static void test_five_minute_late_event_stays_within_psram_window(void)
{
    run_late_event_case(300U, 270.0, true);
    puts("  five-minute recording keeps bounded PSRAM and finds event at 270 s -- OK");
}

static void test_gap_at_four_seconds_rejects_whole_recording(void)
{
    reset_fakes();
    prepare_recording_with_event(30U, 25.0);
    g_fixture_gap_index = 4U * 500U;
    localization_calculate_config_t config = default_test_config();
    config.sample_rate_hz = 500U;
    localization_calculate_summary_t summary;
    assert(localization_calculate_run("OBJ01", "S000052", &config, 52052ULL, &summary) == ESP_OK);
    assert(g_recorded_count == 1U && g_pick_write_calls == 0U);
    assert(strcmp(g_recorded[0].rejection_reason, LOCALIZATION_RUN_REJECTION_REASON_SESSION_GAP) == 0);
    free_fixture_frames();
    puts("  known gap at 4 s rejects recording before late event picks -- OK");
}

static void test_gap_after_prefix_rejects_whole_recording(void)
{
    reset_fakes();
    prepare_recording_with_event(30U, 5.0);
    g_fixture_gap_index = 12U * 500U;
    localization_calculate_config_t config = default_test_config();
    config.sample_rate_hz = 500U;
    localization_calculate_summary_t summary;
    assert(localization_calculate_run("OBJ01", "S000053", &config, 52053ULL, &summary) == ESP_OK);
    const localization_run_record_t *analysis = find_record("localization_analysis");
    assert(analysis != NULL);
    assert(strcmp(analysis->rejection_reason, LOCALIZATION_RUN_REJECTION_REASON_SESSION_GAP) == 0);
    assert(g_pick_write_calls == 0U);
    free_fixture_frames();
    puts("  known gap at 12 s rejects whole recording despite early event -- OK");
}


/* Portable best-effort recursive mkdir for the handful of fixed segments
 * this test needs under the fake STORAGE_MANAGER_OBJECTS_PATH root. */
static void ensure_dir(const char *path)
{
    (void)mkdir(path, 0775);
}

static void prepare_session_directory(const char *object_id, const char *session_id)
{
    char path[256];
    ensure_dir(STORAGE_MANAGER_MOUNT_POINT);
    ensure_dir(STORAGE_MANAGER_ROOT_PATH);
    ensure_dir(STORAGE_MANAGER_OBJECTS_PATH);
    snprintf(path, sizeof(path), "%s/%s", STORAGE_MANAGER_OBJECTS_PATH, object_id);
    ensure_dir(path);
    snprintf(path, sizeof(path), "%s/%s/sessions", STORAGE_MANAGER_OBJECTS_PATH, object_id);
    ensure_dir(path);
    snprintf(path, sizeof(path), "%s/%s/sessions/%s", STORAGE_MANAGER_OBJECTS_PATH, object_id, session_id);
    ensure_dir(path);
}


static uint32_t test_crc32(const uint8_t *bytes, size_t length)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0U; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return crc ^ 0xFFFFFFFFU;
}


static void test_write_u32_le(uint8_t *bytes, size_t offset, uint32_t value)
{
    bytes[offset] = (uint8_t)value;
    bytes[offset + 1U] = (uint8_t)(value >> 8U);
    bytes[offset + 2U] = (uint8_t)(value >> 16U);
    bytes[offset + 3U] = (uint8_t)(value >> 24U);
}


static void test_scan_frame_id(const uint8_t *scan_bytes, size_t scan_length, char frame_id[32])
{
    (void)snprintf(frame_id, 32U, "scan-crc32:%08x", (unsigned)test_crc32(scan_bytes, scan_length));
}


static void write_test_scan_file(
    const char *object_id,
    const char *session_id,
    const uint8_t *scan_bytes,
    size_t scan_length
)
{
    char path[256];
    const int written = snprintf(
        path, sizeof(path), "%s/%s/sessions/%s/scan-v1.bin",
        STORAGE_MANAGER_OBJECTS_PATH, object_id, session_id
    );
    assert(written > 0 && (size_t)written < sizeof(path));
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(scan_bytes, 1U, scan_length, file) == scan_length);
    assert(fclose(file) == 0);
}


static void write_test_occupancy_file(
    const char *object_id,
    const char *session_id,
    const char *frame_id,
    bool reflected
)
{
    char path[256];
    const int path_written = snprintf(
        path, sizeof(path), "%s/%s/sessions/%s/occupancy-v1.bin",
        STORAGE_MANAGER_OBJECTS_PATH, object_id, session_id
    );
    assert(path_written > 0 && (size_t)path_written < sizeof(path));

    char header[1024];
    const char *transform = reflected
        ? "[-1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]"
        : "[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]";
    const int header_written = snprintf(
        header, sizeof(header),
        "{\"schema\":2,\"frame_id\":\"%s\",\"voxel_size_m\":0.5,"
        "\"below_surface_m\":1,\"free_count\":0,\"occupied_count\":0,"
        "\"created_at_ms\":234567,\"registration\":{\"source\":\"test\","
        "\"frame\":\"project_z_up\",\"geometry_computed_at_ms\":1000,"
        "\"residual_m\":0.01,\"threshold_m\":0.05,"
        "\"sensor_to_scan\":%s}}",
        frame_id, transform
    );
    assert(header_written > 0 && (size_t)header_written < sizeof(header));

    const size_t header_length = (size_t)header_written;
    const size_t artifact_length = 20U + header_length;
    uint8_t *artifact = calloc(artifact_length, sizeof(*artifact));
    assert(artifact != NULL);
    memcpy(artifact, "GOCCUPY1", 8U);
    test_write_u32_le(artifact, 8U, (uint32_t)header_length);
    test_write_u32_le(artifact, 12U, 0U);
    memcpy(artifact + 20U, header, header_length);
    test_write_u32_le(artifact, 16U, test_crc32(artifact + 20U, header_length));

    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(artifact, 1U, artifact_length, file) == artifact_length);
    assert(fclose(file) == 0);
    free(artifact);
}


static void remove_test_scan_artifacts(const char *object_id, const char *session_id)
{
    char path[256];
    (void)snprintf(
        path, sizeof(path), "%s/%s/sessions/%s/occupancy-v1.bin",
        STORAGE_MANAGER_OBJECTS_PATH, object_id, session_id
    );
    (void)remove(path);
    (void)snprintf(
        path, sizeof(path), "%s/%s/sessions/%s/scan-v1.bin",
        STORAGE_MANAGER_OBJECTS_PATH, object_id, session_id
    );
    (void)remove(path);
}


static void assert_scan_identity_rejected(
    const char *object_id,
    const char *session_id,
    const char *reason_token
)
{
    localization_calculate_config_t config = default_test_config();
    localization_calculate_summary_t summary;
    const esp_err_t err = localization_calculate_run(
        object_id, session_id, &config, 51000ULL, &summary
    );
    assert(err == ESP_OK);
    assert(g_recorded_count == 21U);

    static const char *const unconstrained_methods[] = {
        "tdoa_gauss_newton", "traveltime_inversion", "joint_fusion"
    };
    for (size_t i = 0U; i < sizeof(unconstrained_methods) / sizeof(unconstrained_methods[0]); ++i) {
        const localization_run_record_t *record = find_record(unconstrained_methods[i]);
        assert(record != NULL);
        assert(strstr(record->rejection_reason, reason_token) == NULL);
    }

    static const char *const scan_methods[] = {
        "traveltime_scan_constrained", "semblance_scan_constrained",
        "tdcas_scan_constrained", "psafuvs_scan_constrained", "suea_scan_voids",
        "eikonal_scan_voids", "semblance_eikonal_voids", "bayesian_scan_voids"
    };
    for (size_t i = 0U; i < sizeof(scan_methods) / sizeof(scan_methods[0]); ++i) {
        const localization_run_record_t *record = find_record(scan_methods[i]);
        assert(record != NULL && !record->solved);
        assert(strstr(record->rejection_reason, reason_token) != NULL);
        assert(record->scan_provenance_present);
        assert(!record->scan_constraint_applied);
        assert(record->scan_frame_id[0] == '\0');
        assert(record->occupancy_created_at_ms == 0U);
        assert(record->registration_geometry_computed_at_ms == 0U);
    }
}


static void test_scan_identity_mismatch_and_unavailable_fail_closed(void)
{
    const size_t scan_length = 9000U;
    uint8_t *scan_a = malloc(scan_length);
    uint8_t *scan_b = malloc(scan_length);
    assert(scan_a != NULL && scan_b != NULL);
    for (size_t i = 0U; i < scan_length; ++i) {
        scan_a[i] = (uint8_t)(i % 251U);
    }
    memcpy(scan_b, scan_a, scan_length);
    scan_b[scan_length - 1U] ^= 0x5AU;
    char occupancy_frame_id[32];
    test_scan_frame_id(scan_a, scan_length, occupancy_frame_id);

    reset_fakes();
    build_fixture(4U, &s_source, TEST_SPEED_MPS, TEST_EMISSION_S, 1.0f, 0.02f, true);
    const char *mismatch_object = "OBJSCANB";
    const char *mismatch_session = "S098001";
    prepare_session_directory(mismatch_object, mismatch_session);
    write_test_occupancy_file(mismatch_object, mismatch_session, occupancy_frame_id, false);
    write_test_scan_file(mismatch_object, mismatch_session, scan_b, scan_length);
    assert_scan_identity_rejected(
        mismatch_object, mismatch_session, "scan_generation_mismatch"
    );
    remove_test_scan_artifacts(mismatch_object, mismatch_session);

    reset_fakes();
    build_fixture(4U, &s_source, TEST_SPEED_MPS, TEST_EMISSION_S, 1.0f, 0.02f, true);
    const char *unavailable_object = "OBJSCANMISS";
    const char *unavailable_session = "S098002";
    prepare_session_directory(unavailable_object, unavailable_session);
    write_test_occupancy_file(unavailable_object, unavailable_session, occupancy_frame_id, false);
    assert_scan_identity_rejected(
        unavailable_object, unavailable_session, "scan_identity_unavailable"
    );
    remove_test_scan_artifacts(unavailable_object, unavailable_session);

    reset_fakes();
    build_fixture(4U, &s_source, TEST_SPEED_MPS, TEST_EMISSION_S, 1.0f, 0.02f, true);
    const char *matching_object = "OBJSCANMATCH";
    const char *matching_session = "S098003";
    prepare_session_directory(matching_object, matching_session);
    write_test_occupancy_file(matching_object, matching_session, occupancy_frame_id, false);
    write_test_scan_file(matching_object, matching_session, scan_a, scan_length);
    localization_calculate_config_t config = default_test_config();
    localization_calculate_summary_t summary;
    assert(localization_calculate_run(
        matching_object, matching_session, &config, 52000ULL, &summary
    ) == ESP_OK);
    const localization_run_record_t *traveltime = find_record("traveltime_scan_constrained");
    assert(traveltime != NULL && traveltime->scan_provenance_present);
    assert(traveltime->scan_constraint_applied);
    assert(strcmp(traveltime->scan_frame_id, occupancy_frame_id) == 0);
    assert(traveltime->occupancy_created_at_ms == 234567U);
    assert(traveltime->registration_geometry_computed_at_ms == 1000U);
    const localization_run_record_t *joint = find_record("joint_fusion");
    assert(joint != NULL && !joint->scan_constraint_applied);
    assert(joint->scan_frame_id[0] == '\0');
    assert(joint->occupancy_created_at_ms == 0U);
    assert(joint->registration_geometry_computed_at_ms == 0U);
    remove_test_scan_artifacts(matching_object, matching_session);

    reset_fakes();
    build_fixture(4U, &s_source, TEST_SPEED_MPS, TEST_EMISSION_S, 1.0f, 0.02f, true);
    const char *reflected_object = "OBJSCANREFL";
    const char *reflected_session = "S098004";
    prepare_session_directory(reflected_object, reflected_session);
    write_test_occupancy_file(reflected_object, reflected_session, occupancy_frame_id, true);
    write_test_scan_file(reflected_object, reflected_session, scan_a, scan_length);
    localization_calculate_config_t reflected_config = default_test_config();
    localization_calculate_summary_t reflected_summary;
    assert(localization_calculate_run(
        reflected_object, reflected_session, &reflected_config, 52001ULL, &reflected_summary
    ) == ESP_OK);
    static const char *const scan_methods[] = {
        "traveltime_scan_constrained", "semblance_scan_constrained",
        "tdcas_scan_constrained", "psafuvs_scan_constrained", "suea_scan_voids",
        "eikonal_scan_voids", "semblance_eikonal_voids", "bayesian_scan_voids"
    };
    for (size_t i = 0U; i < sizeof(scan_methods) / sizeof(scan_methods[0]); ++i) {
        const localization_run_record_t *record = find_record(scan_methods[i]);
        assert(record != NULL && !record->solved);
        assert(strcmp(record->rejection_reason,
                      LOCALIZATION_RUN_REJECTION_REASON_REFLECTED_TRANSFORM) == 0);
        assert(record->scan_provenance_present && !record->scan_constraint_applied);
    }
    remove_test_scan_artifacts(reflected_object, reflected_session);

    free(scan_a);
    free(scan_b);
    puts("  scan generation mismatch, unavailable identity, and reflected registration persist their reasons");
}


/*
 * A session with an occupancy artifact on disk that is not a valid/usable
 * scan (garbage bytes: fails scan_occupancy_parse's magic/CRC check, same
 * code path as "no valid sensor<->scan registration for this session").
 * The 8 scan-constrained variants must all be persisted as unsolved with
 * the "constraint not applied" reason, never silently solved and never
 * fused into the joint result.
 */
static void test_unusable_scan_reports_constraint_not_applied(void)
{
    static const char *const scan_methods[] = {
        "traveltime_scan_constrained", "semblance_scan_constrained",
        "tdcas_scan_constrained", "psafuvs_scan_constrained", "suea_scan_voids",
        "eikonal_scan_voids", "semblance_eikonal_voids", "bayesian_scan_voids"
    };

    reset_fakes();
    build_fixture(4U, &s_source, TEST_SPEED_MPS, TEST_EMISSION_S, 1.0f, 0.02f, true);

    const char *object_id = "OBJGARB";
    const char *session_id = "S099999";
    prepare_session_directory(object_id, session_id);

    char occupancy_path[256];
    snprintf(
        occupancy_path, sizeof(occupancy_path), "%s/%s/sessions/%s/occupancy-v1.bin",
        STORAGE_MANAGER_OBJECTS_PATH, object_id, session_id
    );
    FILE *garbage = fopen(occupancy_path, "wb");
    assert(garbage != NULL);
    const char garbage_bytes[] = "not a valid scan occupancy artifact";
    fwrite(garbage_bytes, 1U, sizeof(garbage_bytes), garbage);
    fclose(garbage);

    localization_calculate_config_t config = default_test_config();
    localization_calculate_summary_t summary;
    const esp_err_t err = localization_calculate_run(
        object_id, session_id, &config, 50000ULL, &summary
    );

    assert(err == ESP_OK);
    /* 12 unconstrained + 8 scan-constrained + 1 joint. */
    assert(g_recorded_count == 21U);
    assert(summary.methods_total == 21U && summary.methods_completed == 21U);

    static const char *const not_applied_reason =
        "scan occupancy artifact unreadable or invalid: constraint not applied";
    for (size_t i = 0U; i < sizeof(scan_methods) / sizeof(scan_methods[0]); ++i) {
        const localization_run_record_t *record = find_record(scan_methods[i]);
        assert(record != NULL);
        assert(!record->solved);
        assert(strcmp(record->rejection_reason, not_applied_reason) == 0);
        assert(record->failure_class == LOCALIZATION_FAILURE_CLASS_INTERNAL);
        localization_calculate_test_assert_rejection_class(record, LOCALIZATION_FAILURE_CLASS_INTERNAL);
    }

    remove(occupancy_path);
    puts("  unusable scan artifact: 8 scan variants report 'constraint not applied' -- OK");
}


/* ---- Event search: which part of a long recording is analysed --------------------------------------------------- */

/* duration_s of noise at 500 Hz on the tetrahedron fixture, no event yet. */
static void prepare_quiet_recording(uint32_t duration_s)
{
    const uint32_t sample_rate_hz = 500U;
    const size_t recording_frames = (size_t)duration_s * sample_rate_hz;
    build_fixture(4U, &s_source, TEST_SPEED_MPS, TEST_EMISSION_S, 0.0f, 0.0f, false);
    extend_fixture_frames(recording_frames);
    g_fixture_summary.sample_rate_hz = sample_rate_hz;
    for (size_t i = 0U; i < recording_frames; ++i) {
        g_fixture_frames[i].timestamp_us = 5000000LL + (int64_t)i * 2000LL;
        for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
            g_fixture_frames[i].channel_uv[ch] = next_noise_sample(0.02f);
        }
    }
}

/* An event of the fixture source emitted at event_time_s, seen by each channel with its own amplitude (0: not seen). */
static void add_event(double event_time_s, const float amplitude[OBJECT_STORE_POD_COUNT])
{
    for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
        if (amplitude[ch] == 0.0f) continue;
        const double dx = s_source.x_m - s_pod_positions[ch].x_m;
        const double dy = s_source.y_m - s_pod_positions[ch].y_m;
        const double dz = s_source.z_m - s_pod_positions[ch].z_m;
        const double arrival_s = event_time_s + sqrt(dx * dx + dy * dy + dz * dz) / TEST_SPEED_MPS;
        add_ramp_pulse_to_channel(ch, (size_t)llround(arrival_s * 500.0), 20U, amplitude[ch]);
    }
}

static localization_calculate_config_t search_test_config(void)
{
    localization_calculate_config_t config = default_test_config();
    config.sample_rate_hz = 500U;
    config.pick_config.noise_window_samples = 25U;
    config.pick_config.sta_window_samples = 5U;
    config.pick_config.lta_window_samples = 50U;
    config.pick_config.refine_lookback_samples = 5U;
    config.pick_config.refine_lookahead_samples = 5U;
    config.pick_config.snr_signal_window_samples = 20U;
    return config;
}

/* Every accepted analysed pick lies in [t, t + 0.1 s): the analysis window holds the event at t. */
static void assert_analysed_event_at(double t_s, size_t expected_accepted)
{
    size_t accepted = 0U;
    for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
        const geophone_channel_pick_t *pick = &g_pick_record.channels[ch].pick;
        if (pick->status != GEOPHONE_PICK_ACCEPTED) continue;
        ++accepted;
        printf("  [SEARCH] ch%u pick at %.3f s (expected the event at %.3f s)\n",
               (unsigned)ch, (double)pick->arrival_sample_index / 500.0, t_s);
        assert(pick->arrival_sample_index >= (size_t)(t_s * 500.0));
        assert(pick->arrival_sample_index < (size_t)((t_s + 0.1) * 500.0));
    }
    assert(accepted == expected_accepted);
}

/* Two events: the one more channels agree on is analysed, even when it is the weaker one. */
static void test_search_prefers_the_event_more_channels_agree_on(void)
{
    static const float three_strong[4] = { 1.0f, 1.0f, 1.0f, 0.0f };
    static const float four_weak[4] = { 0.4f, 0.4f, 0.4f, 0.4f };
    reset_fakes();
    prepare_quiet_recording(30U);
    add_event(6.0, three_strong);
    add_event(17.0, four_weak);
    localization_calculate_config_t config = search_test_config();
    localization_calculate_summary_t summary;
    assert(localization_calculate_run("OBJ01", "S000061", &config, 52061ULL, &summary) == ESP_OK);
    assert_analysed_event_at(17.0, 4U);
    free_fixture_frames();
    puts("  search: the event four channels agree on beats a stronger three-channel one -- OK");
}

/*
 * Two events seen by the same three channels: the stronger is analysed. A spike on the fourth channel 1.2 s before the
 * weaker event (in both of its search windows) is no arrival of it (the consistency gate rejects it) and must not lend that window its SNR.
 */
static void test_search_tie_goes_to_the_stronger_event_not_to_a_glitch(void)
{
    static const float strong[4] = { 1.0f, 1.0f, 1.0f, 0.0f };
    static const float weak[4] = { 0.5f, 0.5f, 0.5f, 0.0f };
    reset_fakes();
    prepare_quiet_recording(30U);
    add_event(6.0, strong);
    add_event(17.0, weak);
    add_ramp_pulse_to_channel(3U, (size_t)(15.8 * 500.0), 20U, 2.5f);
    localization_calculate_config_t config = search_test_config();
    localization_calculate_summary_t summary;
    assert(localization_calculate_run("OBJ01", "S000062", &config, 52062ULL, &summary) == ESP_OK);
    assert_analysed_event_at(6.0, 3U);
    free_fixture_frames();
    puts("  search: equal channel counts -> the stronger event, a glitch adds no strength -- OK");
}

/* A long recording without any event: the analysis keeps the recording's start and solves nothing. */
static void test_search_without_an_event_analyses_the_start(void)
{
    reset_fakes();
    prepare_quiet_recording(30U);
    localization_calculate_config_t config = search_test_config();
    localization_calculate_summary_t summary;
    assert(localization_calculate_run("OBJ01", "S000063", &config, 52063ULL, &summary) == ESP_OK);
    const localization_run_record_t *tdoa = find_record("tdoa_gauss_newton");
    assert(tdoa != NULL);
    assert(tdoa->analysis_window_start_index == 0U);
    assert(!tdoa->solved);
    free_fixture_frames();
    puts("  search: no event anywhere -> the start is analysed and nothing is solved -- OK");
}

/* ---- A full run with a scan of real FREE / OCCUPIED voxels ------------------------------------------------------ */

/* An occupancy artifact in the WebUI encoder's layout (tests/vectors/scan_occupancy_*): header JSON, then int16 x/y/z
 * voxel indices, FREE first, CRC32 over both. Identity registration against the fixture geometry (computed at 1000). */
static void write_test_occupancy_voxels(
    const char *object_id, const char *session_id, const char *frame_id, double voxel_size_m,
    const int16_t (*free_cells)[3], size_t free_count, const int16_t (*occupied_cells)[3], size_t occupied_count,
    const char *sensor_to_scan)
{
    char path[256];
    const int path_written = snprintf(path, sizeof(path), "%s/%s/sessions/%s/occupancy-v1.bin",
                                      STORAGE_MANAGER_OBJECTS_PATH, object_id, session_id);
    assert(path_written > 0 && (size_t)path_written < sizeof(path));
    char header[1024];
    const int header_written = snprintf(
        header, sizeof(header),
        "{\"schema\":2,\"frame_id\":\"%s\",\"voxel_size_m\":%.3f,\"below_surface_m\":1,"
        "\"free_count\":%u,\"occupied_count\":%u,\"created_at_ms\":345678,"
        "\"registration\":{\"source\":\"test\",\"frame\":\"project_z_up\",\"geometry_computed_at_ms\":1000,"
        "\"residual_m\":0.01,\"threshold_m\":0.05,\"sensor_to_scan\":%s}}",
        frame_id, voxel_size_m, (unsigned)free_count, (unsigned)occupied_count, sensor_to_scan);
    assert(header_written > 0 && (size_t)header_written < sizeof(header));
    const size_t header_length = (size_t)header_written;
    const size_t payload_length = 6U * (free_count + occupied_count);
    const size_t artifact_length = 20U + header_length + payload_length;
    uint8_t *artifact = calloc(artifact_length, 1U);
    assert(artifact != NULL);
    memcpy(artifact, "GOCCUPY1", 8U);
    test_write_u32_le(artifact, 8U, (uint32_t)header_length);
    test_write_u32_le(artifact, 12U, (uint32_t)payload_length);
    memcpy(artifact + 20U, header, header_length);
    uint8_t *cell = artifact + 20U + header_length;
    for (size_t list = 0U; list < 2U; ++list) {
        const int16_t (*cells)[3] = list == 0U ? free_cells : occupied_cells;
        const size_t count = list == 0U ? free_count : occupied_count;
        for (size_t i = 0U; i < count; ++i, cell += 6U) {
            for (size_t axis = 0U; axis < 3U; ++axis) {
                cell[2U * axis] = (uint8_t)((uint16_t)cells[i][axis] & 0xFFU);
                cell[2U * axis + 1U] = (uint8_t)((uint16_t)cells[i][axis] >> 8U);
            }
        }
    }
    test_write_u32_le(artifact, 16U, test_crc32(artifact + 20U, header_length + payload_length));
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(artifact, 1U, artifact_length, file) == artifact_length);
    assert(fclose(file) == 0);
    free(artifact);
}

/* A 3x3x3 block of 0.2 m FREE voxels starting at cell (x0, y0, z0); a 10x10 OCCUPIED slab just below z = 0. */
static size_t make_void_block(int16_t x0, int16_t y0, int16_t z0, int16_t free_cells[27][3], int16_t occupied_cells[100][3])
{
    size_t n = 0U;
    for (int16_t x = 0; x < 3; ++x) {
        for (int16_t y = 0; y < 3; ++y) {
            for (int16_t z = 0; z < 3; ++z, ++n) {
                free_cells[n][0] = (int16_t)(x0 + x);
                free_cells[n][1] = (int16_t)(y0 + y);
                free_cells[n][2] = (int16_t)(z0 + z);
            }
        }
    }
    for (int16_t i = 0; i < 100; ++i) {
        occupied_cells[i][0] = (int16_t)(i % 10);
        occupied_cells[i][1] = (int16_t)(i / 10);
        occupied_cells[i][2] = -1;
    }
    return n;
}

static bool in_void_block(const localization_point3d_t *p, int x0, int y0, int z0)
{
    const int cx = (int)floor(p->x_m / 0.2);
    const int cy = (int)floor(p->y_m / 0.2);
    const int cz = (int)floor(p->z_m / 0.2);
    return cx >= x0 && cx < x0 + 3 && cy >= y0 && cy < y0 + 3 && cz >= z0 && cz < z0 + 3;
}

static double distance_m(const localization_point3d_t *a, const localization_point3d_t *b)
{
    const double dx = a->x_m - b->x_m;
    const double dy = a->y_m - b->y_m;
    const double dz = a->z_m - b->z_m;
    return sqrt(dx * dx + dy * dy + dz * dz);
}

static void run_with_void_block_registered(
    const char *session_id, int16_t x0, int16_t y0, int16_t z0, const char *sensor_to_scan)
{
    const size_t scan_length = 4096U;
    uint8_t *scan = malloc(scan_length);
    assert(scan != NULL);
    for (size_t i = 0U; i < scan_length; ++i) scan[i] = (uint8_t)(i * 7U + (size_t)x0);
    char frame_id[32];
    test_scan_frame_id(scan, scan_length, frame_id);
    int16_t free_cells[27][3];
    int16_t occupied_cells[100][3];
    const size_t free_count = make_void_block(x0, y0, z0, free_cells, occupied_cells);
    reset_fakes();
    build_fixture(4U, &s_source, TEST_SPEED_MPS, TEST_EMISSION_S, 1.0f, 0.02f, true);
    prepare_session_directory("OBJVOXEL", session_id);
    write_test_scan_file("OBJVOXEL", session_id, scan, scan_length);
    write_test_occupancy_voxels("OBJVOXEL", session_id, frame_id, 0.2,
                                (const int16_t (*)[3])free_cells, free_count,
                                (const int16_t (*)[3])occupied_cells, 100U, sensor_to_scan);
    localization_calculate_config_t config = default_test_config();
    localization_calculate_summary_t summary;
    assert(localization_calculate_run("OBJVOXEL", session_id, &config, 52070ULL, &summary) == ESP_OK);
    remove_test_scan_artifacts("OBJVOXEL", session_id);
    free(scan);
}

static void run_with_void_block(const char *session_id, int16_t x0, int16_t y0, int16_t z0)
{
    run_with_void_block_registered(session_id, x0, y0, z0, "[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]");
}

/*
 * REMOTE_ONLY with registered AprilTag positions: the localization geometry IS the scan's project frame (the tag
 * centres), while the occupancy registration still maps the Channel Sounding layout into it (here a 10 m shift). The
 * voids must be read at the geometry's own coordinates; applying the shift again put every candidate 10 m away, outside
 * every scanned voxel, so the scan never constrained a REMOTE_ONLY localization.
 */
static void test_apriltag_geometry_reads_voxels_without_a_second_transform(void)
{
    g_fixture_geometry_source = SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_APRILTAG_REGISTERED;
    run_with_void_block_registered("S098072", 0, 1, 0, "[1,0,0,10,0,1,0,0,0,0,1,0,0,0,0,1]");
    g_fixture_geometry_source = SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_CHANNEL_SOUNDING;
    const localization_run_record_t *free_run = find_record("traveltime_inversion");
    const localization_run_record_t *scan_run = find_record("traveltime_scan_constrained");
    assert(free_run != NULL && scan_run != NULL);
    printf("  [VOXEL] AprilTag geometry: %s (%.2f, %.2f, %.2f) -> constrained (%.2f, %.2f, %.2f) applied %d\n",
           free_run->solved ? "solved" : "unsolved", free_run->source_position.x_m, free_run->source_position.y_m,
           free_run->source_position.z_m, scan_run->source_position.x_m, scan_run->source_position.y_m,
           scan_run->source_position.z_m, (int)scan_run->scan_constraint_applied);
    fflush(stdout);
    assert(free_run->solved && scan_run->solved && scan_run->scan_constraint_applied);
    assert(in_void_block(&free_run->source_position, 0, 1, 0));
    assert(!in_void_block(&scan_run->source_position, 0, 1, 0));
    puts("  AprilTag geometry: the voids constrain the run at the tag-centre coordinates -- OK");
}

/*
 * The scan's voxels reach the math in a full run, not only its "applied" flag: a FREE (scanned empty) block over the
 * true source pushes every candidate-filtered method out of it, while the same block elsewhere leaves them where the
 * unconstrained search is. The earlier full-run scan test carried zero voxels.
 */
static void test_full_run_feeds_free_and_occupied_voxels_to_the_math(void)
{
    static const char *const filtered_pairs[][2] = {
        { "traveltime_inversion", "traveltime_scan_constrained" },
        { "semblance", "semblance_scan_constrained" },
    };
    /* The source (0.25, 0.40, 0.30) is in cell (1, 2, 1): the block from (0, 1, 0) contains it. */
    run_with_void_block("S098070", 0, 1, 0);
    for (size_t i = 0U; i < 2U; ++i) {
        const localization_run_record_t *free_run = find_record(filtered_pairs[i][0]);
        const localization_run_record_t *scan_run = find_record(filtered_pairs[i][1]);
        assert(free_run != NULL && scan_run != NULL);
        printf("  [VOXEL] %s solved %d (%.2f, %.2f, %.2f) -> %s solved %d applied %d (%.2f, %.2f, %.2f)\n",
               filtered_pairs[i][0], (int)free_run->solved, free_run->source_position.x_m,
               free_run->source_position.y_m, free_run->source_position.z_m, filtered_pairs[i][1],
               (int)scan_run->solved, (int)scan_run->scan_constraint_applied, scan_run->source_position.x_m,
               scan_run->source_position.y_m, scan_run->source_position.z_m);
        fflush(stdout);
        assert(free_run->solved && scan_run->solved && scan_run->scan_constraint_applied);
        /* No constrained result is ever inside scanned-empty space. */
        assert(!in_void_block(&scan_run->source_position, 0, 1, 0));
        if (i == 0U) {
            /* Traveltime solves at the source without the scan, so the void must move it. (Semblance focuses
             * elsewhere on this small fixture with or without the scan.) */
            assert(in_void_block(&free_run->source_position, 0, 1, 0));
            assert(distance_m(&free_run->source_position, &scan_run->source_position) >= 0.1);
        }
    }
    const localization_run_record_t *suea_voids = find_record("suea_scan_voids");
    assert(suea_voids != NULL && suea_voids->scan_constraint_applied);

    /* The same block far from the source changes nothing. */
    run_with_void_block("S098071", 6, 6, 6);
    for (size_t i = 0U; i < 2U; ++i) {
        const localization_run_record_t *free_run = find_record(filtered_pairs[i][0]);
        const localization_run_record_t *scan_run = find_record(filtered_pairs[i][1]);
        assert(free_run != NULL && scan_run != NULL && scan_run->solved && scan_run->scan_constraint_applied);
        assert(distance_m(&free_run->source_position, &scan_run->source_position) < 1.0e-9);
    }
    puts("  full run: FREE voxels over the source move the constrained results out, elsewhere they do not -- OK");
}

int main(void)
{
#if defined(SEARCH_SCENARIO) /* one of the new cases alone (run against the previous source too) */
#if SEARCH_SCENARIO == 1
    test_search_prefers_the_event_more_channels_agree_on();
#elif SEARCH_SCENARIO == 2
    test_search_tie_goes_to_the_stronger_event_not_to_a_glitch();
#elif SEARCH_SCENARIO == 3
    test_search_without_an_event_analyses_the_start();
#elif SEARCH_SCENARIO == 4
    test_full_run_feeds_free_and_occupied_voxels_to_the_math();
#elif SEARCH_SCENARIO == 5
    test_apriltag_geometry_reads_voxels_without_a_second_transform();
#endif
    free_fixture_frames();
    return 0;
#else
    const char *p5_scenario = getenv("P5_SCENARIO");
    if (p5_scenario != NULL) {
        if (strcmp(p5_scenario, "r5_1") == 0) {
            test_late_event_in_full_recording_is_localized();
        } else if (strcmp(p5_scenario, "r5_2") == 0) {
            test_event_across_the_search_window_boundary_is_found_once();
        } else if (strcmp(p5_scenario, "r5_3") == 0) {
            test_event_after_45_seconds_preserves_absolute_tdoa();
        } else if (strcmp(p5_scenario, "r5_4") == 0) {
            test_quiet_prefix_does_not_false_fail_late_event();
        } else if (strcmp(p5_scenario, "r5_5_gap_inside") == 0) {
            test_gap_at_four_seconds_rejects_whole_recording();
        } else if (strcmp(p5_scenario, "r5_5_gap_after_prefix") == 0) {
            test_gap_after_prefix_rejects_whole_recording();
        } else if (strcmp(p5_scenario, "r5_6") == 0) {
            test_five_minute_late_event_stays_within_psram_window();
        } else {
            fprintf(stderr, "unknown P5_SCENARIO: %s\n", p5_scenario);
            return 2;
        }
        return 0;
    }

    test_non_finite_velocity_is_rejected_before_io();
    test_sample_windows_rescale_across_session_rates();
    test_run_revalidates_windows_against_session_rate();
    test_insufficient_dimensionality_writes_nothing();
    test_truncated_session_persists_analysis_window();
    test_limited_free_psram_truncates_with_memory_provenance();
    test_insufficient_free_psram_records_internal_failure();
    test_three_pods_planar_no_fabricated_z();
    test_four_pods_clean_event_solves_near_truth();
    test_gap_and_nonmonotonic_timestamps_reject_before_picking();
    test_duplicate_sensor_position_persists_validation_reason();
    test_saturated_and_ambiguous_picks_are_excluded();
    test_secondary_pick_evidence_is_persisted_only_when_enabled();
    test_flat_signal_every_method_unsolved_with_reason();
    test_read_failures_propagate_with_no_partial_writes();
    test_run_store_write_failure_midway_contract();
    test_time_budget_exhausted_skips_remaining_methods();
    test_late_event_in_full_recording_is_localized();
    test_event_across_the_search_window_boundary_is_found_once();
    test_event_after_45_seconds_preserves_absolute_tdoa();
    test_quiet_prefix_does_not_false_fail_late_event();
    test_gap_at_four_seconds_rejects_whole_recording();
    test_gap_after_prefix_rejects_whole_recording();
    test_five_minute_late_event_stays_within_psram_window();
    test_scan_identity_mismatch_and_unavailable_fail_closed();
    test_unusable_scan_reports_constraint_not_applied();
    test_record_stop_then_localize_with_reserved_psram();
    test_search_prefers_the_event_more_channels_agree_on();
    test_search_tie_goes_to_the_stronger_event_not_to_a_glitch();
    test_search_without_an_event_analyses_the_start();
    test_full_run_feeds_free_and_occupied_voxels_to_the_math();
    test_apriltag_geometry_reads_voxels_without_a_second_transform();

    free_fixture_frames();
    puts("localization_calculate_run integration tests passed");
    return 0;
#endif
}
