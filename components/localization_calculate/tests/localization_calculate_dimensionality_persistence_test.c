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
#include "localization_calculate_policy.h"
#include "localization_comparison.h"
#include "localization_dimensionality_policy.h"
#include "localization_run_store.h"
#include "localization_psafuvs.h"
#include "localization_tdcas.h"
#include "object_store.h"
#include "psram_model.h"
#include "scan_occupancy.h"
#include "seismic_buffer.h"
#include "seismic_recorder.h"
#include "storage_manager.h"

#define FIX_SAMPLE_RATE_HZ 50000U
#define FIX_PULSE_WIDTH 80U
#define FIX_WAVELET_FREQUENCY_HZ 1500.0
#define FIX_NOISE_WIDTH 50U
#define FIX_NOISE_AMPLITUDE 0.02f
#define FIX_PULSE_AMPLITUDE 1.0f
#define FIX_P_SPEED_MPS 340.0
#define FIX_S_SPEED_MPS 190.0
#define FIX_EMISSION_S 0.01
#define MATRIX_OBJECT_ID "OBJ_0132"

static const localization_point3d_t s_sensor_positions[OBJECT_STORE_POD_COUNT] = {
    { 0.0, 0.0, 0.0 },
    { 2.0, 0.0, 0.0 },
    { 0.0, 2.0, 0.0 },
    { 0.0, 0.0, 2.0 }
};

static const localization_point3d_t s_planar_source = { 0.7, 0.8, 0.0 };
static const localization_point3d_t s_3d_source = { 0.25, 0.4, 0.3 };

typedef struct {
    const char *name;
    bool solved_planar_3_remote;
    bool solved_3d_4_remote;
} method_expectation_t;

/* Derived from each backend's dimensionality/count contract. TDCAS requires
 * 5 planar / 6 depth-resolving picks; PSAFUVS requires 4 planar / 5 3D picks.
 * The other methods are given clean P picks, plus S picks for the joint P/S
 * backends. */
static const method_expectation_t s_method_expectations[] = {
    { "tdoa_gauss_newton", false, true },
    { "traveltime_inversion", true, true },
    { "semblance", true, true },
    { "energy_baseline", true, true },
    { "tdcas", false, false },
    { "psafuvs", false, false },
    { "suea", true, true },
    { "xcorr_tdoa", true, true },
    { "robust_tdoa", true, true },
    { "psp_joint", true, true },
    { "bayesian", true, true },
    { "eikonal_fmm", true, true },
};

static const char *const s_scan_methods[] = {
    "traveltime_scan_constrained", "semblance_scan_constrained",
    "tdcas_scan_constrained", "psafuvs_scan_constrained", "suea_scan_voids",
    "eikonal_scan_voids", "semblance_eikonal_voids", "bayesian_scan_voids"
};

static object_store_geometry_t g_geometry;
static seismic_frame_t *g_frames;
static size_t g_frame_count;
static seismic_recorder_session_summary_t g_session_summary;
static int64_t g_now_us = 1000000;
static uint32_t g_noise_state;

int64_t esp_timer_get_time(void)
{
    return g_now_us;
}

esp_err_t storage_manager_operation_begin(void)
{
    return ESP_OK;
}

void storage_manager_operation_end(void)
{
}

esp_err_t storage_manager_read_text(const char *path, char *buffer, size_t buffer_size)
{
    if (path == NULL || buffer == NULL || buffer_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    const size_t read_count = fread(buffer, 1U, buffer_size - 1U, file);
    const bool truncated = fgetc(file) != EOF;
    (void)fclose(file);
    if (truncated) {
        return ESP_ERR_INVALID_SIZE;
    }
    buffer[read_count] = '\0';
    return ESP_OK;
}

esp_err_t storage_manager_atomic_write_text(const char *path, const char *text)
{
    if (path == NULL || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return ESP_FAIL;
    }
    const bool ok = fputs(text, file) >= 0;
    return fclose(file) == 0 && ok ? ESP_OK : ESP_FAIL;
}

esp_err_t seismic_recorder_read_session_geometry(
    const char *object_id,
    const char *session_id,
    object_store_geometry_t *geometry
)
{
    (void)object_id;
    (void)session_id;
    if (geometry == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *geometry = g_geometry;
    return ESP_OK;
}

esp_err_t seismic_recorder_get_session_summary(
    const char *object_id,
    const char *session_id,
    seismic_recorder_session_summary_t *summary_out
)
{
    (void)object_id;
    (void)session_id;
    if (summary_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *summary_out = g_session_summary;
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
    if (frames == NULL || frame_count_out == NULL || start_index > g_frame_count || max_frames == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t remaining = g_frame_count - (size_t)start_index;
    const size_t count = remaining < max_frames ? remaining : max_frames;
    memcpy(frames, g_frames + (size_t)start_index, sizeof(*frames) * count);
    for (size_t i = 0U; i < count; ++i) {
        if (source_sample_indices != NULL) source_sample_indices[i] = start_index + (uint64_t)i;
        if (gap_before != NULL) gap_before[i] = false;
    }
    if (raw_stats_out != NULL) {
        memset(raw_stats_out, 0, sizeof(*raw_stats_out));
        raw_stats_out->samples_examined = count;
    }
    *frame_count_out = count;
    if (summary_out != NULL) {
        *summary_out = g_session_summary;
    }
    return ESP_OK;
}

static float noise_sample(void)
{
    g_noise_state ^= g_noise_state << 13;
    g_noise_state ^= g_noise_state >> 17;
    g_noise_state ^= g_noise_state << 5;
    return FIX_NOISE_AMPLITUDE * ((float)(g_noise_state % 20001U) / 10000.0f - 1.0f);
}

static void add_pulse(size_t channel, size_t start)
{
    const size_t half_width = FIX_PULSE_WIDTH / 2U;
    const size_t first = start > half_width ? start - half_width : 0U;
    const double pi = 3.141592653589793;
    for (size_t i = 0U; i < FIX_PULSE_WIDTH && first + i < g_frame_count; ++i) {
        const double t = ((double)i - (double)half_width) / (double)FIX_SAMPLE_RATE_HZ;
        const double a = pi * FIX_WAVELET_FREQUENCY_HZ * t;
        const double wavelet = (1.0 - 2.0 * a * a) * exp(-a * a);
        g_frames[first + i].channel_uv[channel] += (float)(FIX_PULSE_AMPLITUDE * wavelet);
    }
}

static void build_fixture(size_t remote_count, const localization_point3d_t *source)
{
    memset(&g_geometry, 0, sizeof(g_geometry));
    size_t p_index[OBJECT_STORE_POD_COUNT] = { 0U };
    size_t s_index[OBJECT_STORE_POD_COUNT] = { 0U };
    g_frame_count = 0U;

    for (size_t channel = 0U; channel < remote_count; ++channel) {
        g_geometry.pods[channel].computed = true;
        g_geometry.pods[channel].leveled = true;
        g_geometry.pods[channel].x_mm = (int32_t)llround(s_sensor_positions[channel].x_m * 1000.0);
        g_geometry.pods[channel].y_mm = (int32_t)llround(s_sensor_positions[channel].y_m * 1000.0);
        g_geometry.pods[channel].z_mm = (int32_t)llround(s_sensor_positions[channel].z_m * 1000.0);
        g_geometry.pods[channel].computed_at_ms = 1000U;

        const double dx = source->x_m - s_sensor_positions[channel].x_m;
        const double dy = source->y_m - s_sensor_positions[channel].y_m;
        const double dz = source->z_m - s_sensor_positions[channel].z_m;
        const double distance = sqrt(dx * dx + dy * dy + dz * dz);
        p_index[channel] = (size_t)llround(
            (FIX_EMISSION_S + distance / FIX_P_SPEED_MPS) * (double)FIX_SAMPLE_RATE_HZ
        );
        s_index[channel] = (size_t)llround(
            (FIX_EMISSION_S + distance / FIX_S_SPEED_MPS) * (double)FIX_SAMPLE_RATE_HZ
        );
        const size_t needed = s_index[channel] + FIX_PULSE_WIDTH + FIX_NOISE_WIDTH + 100U;
        if (needed > g_frame_count) {
            g_frame_count = needed;
        }
    }
    assert(g_frame_count > 0U);
    free(g_frames);
    g_frames = calloc(g_frame_count, sizeof(*g_frames));
    assert(g_frames != NULL);

    for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
        g_noise_state = 0x9E3779B9U ^ ((uint32_t)channel * 0x85EBCA6BU + 1U);
        for (size_t i = 0U; i < g_frame_count; ++i) {
            g_frames[i].channel_uv[channel] = noise_sample();
        }
        if (channel < remote_count) {
            add_pulse(channel, p_index[channel]);
            add_pulse(channel, s_index[channel]);
        }
    }
    for (size_t i = 0U; i < g_frame_count; ++i) {
        g_frames[i].timestamp_us = 7000000LL + (int64_t)i * 20LL;
    }

    memset(&g_session_summary, 0, sizeof(g_session_summary));
    g_session_summary.sample_rate_hz = FIX_SAMPLE_RATE_HZ;
    g_session_summary.channel_count = SEISMIC_CHANNEL_COUNT;
    g_session_summary.samples_written = g_frame_count;
    g_session_summary.complete = true;
    g_session_summary.readable = true;
}

static localization_calculate_config_t fixture_config(void)
{
    localization_calculate_config_t config;
    memset(&config, 0, sizeof(config));
    config.propagation_speed_mps = FIX_P_SPEED_MPS;
    config.propagation_speed_uncertainty_mps = 20.0;
    (void)snprintf(config.propagation_model_name, sizeof(config.propagation_model_name), "max132-synthetic");
    config.timing_uncertainty_us = 400.0;
    config.sample_rate_hz = FIX_SAMPLE_RATE_HZ;
    config.pick_config.noise_window_samples = FIX_NOISE_WIDTH;
    config.pick_config.sta_window_samples = 10U;
    config.pick_config.lta_window_samples = 250U;
    config.pick_config.sta_lta_trigger_ratio = 3.0f;
    config.pick_config.refine_lookback_samples = 10U;
    config.pick_config.refine_lookahead_samples = 10U;
    config.pick_config.refine_amplitude_fraction = 0.5f;
    config.pick_config.snr_signal_window_samples = FIX_PULSE_WIDTH;
    config.pick_config.minimum_snr = 3.0f;
    config.semblance_window_half_span_samples = 10U;
    config.s_pick_enabled = true;
    config.s_pick_config.min_separation_samples = 40U;
    config.s_pick_config.max_separation_samples = 1500U;
    config.s_pick_config.sta_window_samples = 10U;
    config.s_pick_config.lta_window_samples = 50U;
    config.s_pick_config.sta_lta_trigger_ratio = 3.0f;
    config.s_pick_config.minimum_snr = 3.0f;
    return config;
}

static void ensure_dir(const char *path)
{
    (void)mkdir(path, 0775);
}

static void prepare_session_directory(const char *session_id)
{
    char path[256];
    ensure_dir(STORAGE_MANAGER_MOUNT_POINT);
    ensure_dir(STORAGE_MANAGER_ROOT_PATH);
    ensure_dir(STORAGE_MANAGER_OBJECTS_PATH);
    (void)snprintf(path, sizeof(path), "%s/%s", STORAGE_MANAGER_OBJECTS_PATH, MATRIX_OBJECT_ID);
    ensure_dir(path);
    (void)snprintf(path, sizeof(path), "%s/%s/sessions", STORAGE_MANAGER_OBJECTS_PATH, MATRIX_OBJECT_ID);
    ensure_dir(path);
    (void)snprintf(
        path, sizeof(path), "%s/%s/sessions/%s", STORAGE_MANAGER_OBJECTS_PATH, MATRIX_OBJECT_ID, session_id
    );
    ensure_dir(path);
}

static void write_unusable_scan(const char *session_id)
{
    char path[256];
    (void)snprintf(
        path, sizeof(path), "%s/%s/sessions/%s/occupancy-v1.bin",
        STORAGE_MANAGER_OBJECTS_PATH, MATRIX_OBJECT_ID, session_id
    );
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    static const char bytes[] = "invalid test-owned occupancy";
    assert(fwrite(bytes, 1U, sizeof(bytes), file) == sizeof(bytes));
    assert(fclose(file) == 0);
}

static const localization_run_record_t *find_record(
    const localization_run_record_t *records,
    size_t count,
    const char *method_name
)
{
    const localization_run_record_t *found = NULL;
    for (size_t i = 0U; i < count; ++i) {
        if (strcmp(records[i].method_name, method_name) == 0) {
            assert(found == NULL);
            found = &records[i];
        }
    }
    return found;
}

static void assert_zero_position(const localization_run_record_t *record)
{
    assert(record->source_position.x_m == 0.0);
    assert(record->source_position.y_m == 0.0);
    assert(record->source_position.z_m == 0.0);
}

static void print_record(const localization_run_record_t *record, const char *layout)
{
    printf(
        "%s %s %s%s%s\n", layout, record->method_name, record->solved ? "solved" : "skipped",
        record->solved ? "" : ": ", record->solved ? "" : record->rejection_reason
    );
}

static void run_layout(
    size_t remote_count,
    const localization_point3d_t *source,
    localization_dimensionality_t expected_dimensionality,
    bool planar_layout,
    const char *session_id,
    uint64_t computed_at_ms
)
{
    prepare_session_directory(session_id);
    write_unusable_scan(session_id);
    build_fixture(remote_count, source);

    localization_calculate_config_t config = fixture_config();
    localization_calculate_summary_t summary;
    const esp_err_t calculate_err = localization_calculate_run(
        MATRIX_OBJECT_ID, session_id, &config, computed_at_ms, &summary
    );
    if (calculate_err != ESP_OK) {
        (void)fprintf(stderr, "calculate failed for %s: esp_err_t=%d\n", session_id, (int)calculate_err);
    }
    assert(calculate_err == ESP_OK);
    assert(summary.participating_channel_count == remote_count);
    assert(summary.methods_total == 21U && summary.methods_completed == 21U);

    localization_run_record_t records[LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION];
    size_t count = 0U;
    assert(localization_run_store_list(
        MATRIX_OBJECT_ID, session_id, records, LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION, &count
    ) == ESP_OK);
    assert(count == 21U);

    const char *layout = planar_layout ? "3-REMOTE-planar" : "4-REMOTE-3D";
    for (size_t i = 0U; i < sizeof(s_method_expectations) / sizeof(s_method_expectations[0]); ++i) {
        const localization_run_record_t *record = find_record(records, count, s_method_expectations[i].name);
        assert(record != NULL);
        print_record(record, layout);
    }
    for (size_t i = 0U; i < sizeof(s_scan_methods) / sizeof(s_scan_methods[0]); ++i) {
        const localization_run_record_t *record = find_record(records, count, s_scan_methods[i]);
        assert(record != NULL);
        print_record(record, layout);
    }
    fflush(stdout);

    /* ponytail: O(n^2) uniqueness check over this fixed 21-record matrix;
     * use a set only if the production method inventory grows materially. */
    for (size_t i = 0U; i < count; ++i) {
        assert(records[i].dimensionality == expected_dimensionality);
        for (size_t j = i + 1U; j < count; ++j) {
            assert(strcmp(records[i].method_name, records[j].method_name) != 0);
        }
    }

    for (size_t i = 0U; i < sizeof(s_method_expectations) / sizeof(s_method_expectations[0]); ++i) {
        const method_expectation_t *expected = &s_method_expectations[i];
        const localization_run_record_t *record = find_record(records, count, expected->name);
        const bool expected_solved = planar_layout
            ? expected->solved_planar_3_remote
            : expected->solved_3d_4_remote;
        assert(record != NULL && record->kind == LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD);
        assert(record->solved == expected_solved);
        if (record->solved) {
            assert(isfinite(record->source_position.x_m));
            assert(isfinite(record->source_position.y_m));
            assert(isfinite(record->source_position.z_m));
        } else {
            assert(record->rejection_reason[0] != '\0');
            assert_zero_position(record);
            if (!planar_layout && !expected_solved && strcmp(expected->name, "xcorr_tdoa") == 0) {
                assert(strstr(record->rejection_reason, "too few for a 3D fit") != NULL);
            }
        }
    }

    const localization_run_record_t *tdcas = find_record(records, count, "tdcas");
    const localization_run_record_t *psafuvs = find_record(records, count, "psafuvs");
    assert(LOCALIZATION_TDCAS_MIN_SENSORS_PLANAR == 5U);
    assert(LOCALIZATION_TDCAS_MIN_SENSORS_3D == 6U);
    assert(LOCALIZATION_PSAFUVS_MIN_SENSORS_PLANAR == 4U);
    assert(LOCALIZATION_PSAFUVS_MIN_SENSORS_3D == 5U);
    assert(strstr(tdcas->rejection_reason, planar_layout ? "needs 5" : "needs 6") != NULL);
    assert(strstr(tdcas->rejection_reason, planar_layout ? "have 3" : "have 4") != NULL);
    assert(strcmp(
        psafuvs->rejection_reason,
        planar_layout
            ? "PSAFUVS needs 4 channels with accepted P and S picks on a planar array"
            : "PSAFUVS needs 5 channels with accepted P and S picks on a depth-resolving array"
    ) == 0);
    if (planar_layout) {
        const localization_run_record_t *tdoa = find_record(records, count, "tdoa_gauss_newton");
        assert(!tdoa->solved && strstr(tdoa->rejection_reason, "3D") != NULL);
    }

    static const char scan_skip_reason[] =
        "scan occupancy artifact unreadable or invalid: constraint not applied";
    for (size_t i = 0U; i < sizeof(s_scan_methods) / sizeof(s_scan_methods[0]); ++i) {
        const localization_run_record_t *record = find_record(records, count, s_scan_methods[i]);
        assert(record != NULL && record->kind == LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD);
        assert(!record->solved);
        assert(strcmp(record->rejection_reason, scan_skip_reason) == 0);
        assert_zero_position(record);
    }

    localization_comparison_method_result_t comparison_methods[12];
    for (size_t i = 0U; i < sizeof(s_method_expectations) / sizeof(s_method_expectations[0]); ++i) {
        const localization_run_record_t *record = find_record(records, count, s_method_expectations[i].name);
        localization_calculate_run_record_to_comparison_method(record, &comparison_methods[i]);
        if (!record->solved) {
            assert(!comparison_methods[i].solved);
            assert(comparison_methods[i].rejection_reason[0] != '\0');
        }
    }
    localization_comparison_result_t recomputed_fusion;
    assert(localization_comparison_fuse(
        comparison_methods, sizeof(comparison_methods) / sizeof(comparison_methods[0]), &recomputed_fusion
    ) == ESP_OK);
    const localization_run_record_t *joint = find_record(records, count, "joint_fusion");
    assert(joint != NULL && joint->kind == LOCALIZATION_RUN_KIND_JOINT_FUSION);
    assert(joint->solved && recomputed_fusion.joint_found);
    assert(fabs(joint->source_position.x_m - recomputed_fusion.joint_position.x_m) < 1.0e-7);
    assert(fabs(joint->source_position.y_m - recomputed_fusion.joint_position.y_m) < 1.0e-7);
    assert(fabs(joint->source_position.z_m - recomputed_fusion.joint_position.z_m) < 1.0e-7);
    assert(fabs(
        joint->confidence_score - recomputed_fusion.joint_total_quality /
            (double)recomputed_fusion.joint_methods_used_count
    ) < 1.0e-7);
}

int main(void)
{
    assert(LOCALIZATION_PSAFUVS_MIN_SENSORS_PLANAR == 4U);
    assert(LOCALIZATION_PSAFUVS_MIN_SENSORS_3D == 5U);
    run_layout(
        3U, &s_planar_source, LOCALIZATION_DIMENSIONALITY_PLANAR_2D, true,
        "S013205", 132005U
    );
    free(g_frames);
    g_frames = NULL;
    run_layout(
        4U, &s_3d_source, LOCALIZATION_DIMENSIONALITY_CAPABLE_3D, false,
        "S013206", 132006U
    );
    free(g_frames);
    puts("MAX-132 D3 dimensionality persistence matrix passed");
    return 0;
}
