#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "localization_calculate.h"
#include "localization_calculate_policy.h"
#include "localization_run_store.h"
#include "object_store.h"
#include "scan_occupancy.h"
#include "seismic_buffer.h"
#include "seismic_recorder.h"
#include "storage_manager.h"

#define ARRAY_COUNT(items) (sizeof(items) / sizeof((items)[0]))

static object_store_geometry_t g_fake_geometry;
static seismic_recorder_session_summary_t g_fake_summary;
static seismic_frame_t g_single_frame;
static size_t g_geometry_reads;
static size_t g_summary_reads;
static size_t g_range_reads;
static size_t g_run_writes;

int64_t esp_timer_get_time(void)
{
    return 1000000;
}

esp_err_t storage_manager_operation_begin(void)
{
    return ESP_OK;
}

void storage_manager_operation_end(void)
{
}

esp_err_t seismic_recorder_read_session_geometry(
    const char *object_id,
    const char *session_id,
    object_store_geometry_t *geometry
)
{
    (void)object_id;
    (void)session_id;
    ++g_geometry_reads;
    *geometry = g_fake_geometry;
    return ESP_OK;
}

esp_err_t seismic_recorder_read_session_localization_geometry(
    const char *object_id,
    const char *session_id,
    object_store_geometry_t *geometry,
    seismic_recorder_localization_geometry_source_t *source,
    esp_err_t *registered_status
)
{
    if (source != NULL) *source = SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_CHANNEL_SOUNDING;
    if (registered_status != NULL) *registered_status = ESP_OK;
    return seismic_recorder_read_session_geometry(object_id, session_id, geometry);
}

esp_err_t seismic_recorder_read_session_tag_layout(const char *object_id, const char *session_id, seismic_recorder_tag_layout_t *layout)
{
    (void)object_id; (void)session_id;
    if (layout != NULL) memset(layout, 0, sizeof(*layout));
    return ESP_ERR_NOT_FOUND;
}

esp_err_t seismic_recorder_get_session_summary(
    const char *object_id,
    const char *session_id,
    seismic_recorder_session_summary_t *summary_out
)
{
    (void)object_id;
    (void)session_id;
    ++g_summary_reads;
    *summary_out = g_fake_summary;
    return ESP_OK;
}

esp_err_t seismic_recorder_read_session_range(
    const char *object_id,
    const char *session_id,
    uint64_t start_index,
    size_t max_frames,
    seismic_frame_t *frames,
    uint64_t *source_sample_indices,
    bool *gap_before,
    size_t *frame_count_out,
    seismic_recorder_session_summary_t *summary_out
)
{
    (void)object_id;
    (void)session_id;
    (void)source_sample_indices;
    ++g_range_reads;
    if (frames == NULL || frame_count_out == NULL || max_frames == 0U || start_index > 1U) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t available = start_index == 0U ? 1U : 0U;
    const size_t copied = available < max_frames ? available : max_frames;
    if (copied > 0U) {
        frames[0] = g_single_frame;
    }
    *frame_count_out = copied;
    if (gap_before != NULL) {
        *gap_before = false;
    }
    if (summary_out != NULL) {
        *summary_out = g_fake_summary;
    }
    return ESP_OK;
}

esp_err_t localization_run_store_write(
    const char *object_id,
    const char *session_id,
    const localization_run_record_t *record
)
{
    (void)object_id;
    (void)session_id;
    (void)record;
    ++g_run_writes;
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
    (void)record;
    return ESP_OK;
}

static void reset_fakes(size_t sensor_count, size_t sample_count)
{
    assert(sensor_count <= OBJECT_STORE_POD_COUNT);
    memset(&g_fake_geometry, 0, sizeof(g_fake_geometry));
    memset(&g_fake_summary, 0, sizeof(g_fake_summary));
    memset(&g_single_frame, 0, sizeof(g_single_frame));
    g_geometry_reads = 0U;
    g_summary_reads = 0U;
    g_range_reads = 0U;
    g_run_writes = 0U;
    for (size_t i = 0U; i < sensor_count; ++i) {
        g_fake_geometry.pods[i].computed = true;
    }
    g_fake_summary.samples_written = sample_count;
    g_fake_summary.sample_rate_hz = 500U;
    g_fake_summary.readable = true;
    g_fake_summary.complete = true;
}

static localization_calculate_config_t valid_calculate_config(void)
{
    localization_calculate_config_t config;
    memset(&config, 0, sizeof(config));
    config.propagation_speed_mps = 340.0;
    config.propagation_speed_uncertainty_mps = 0.0;
    config.timing_uncertainty_us = 60.0;
    config.sample_rate_hz = 500U;
    config.pick_config.noise_window_samples = 200U;
    config.pick_config.sta_window_samples = 40U;
    config.pick_config.lta_window_samples = 400U;
    config.pick_config.sta_lta_trigger_ratio = 3.0f;
    config.pick_config.refine_lookback_samples = 40U;
    config.pick_config.refine_lookahead_samples = 40U;
    config.pick_config.refine_amplitude_fraction = 0.5f;
    config.pick_config.snr_signal_window_samples = 80U;
    config.pick_config.minimum_snr = 3.0f;
    config.semblance_window_half_span_samples = 40U;
    return config;
}

typedef enum
{
    CALC_NULL_OBJECT,
    CALC_NULL_SESSION,
    CALC_NULL_CONFIG,
    CALC_NULL_SUMMARY,
    CALC_SPEED_ZERO,
    CALC_SPEED_NEGATIVE,
    CALC_SPEED_NAN,
    CALC_SPEED_POS_INF,
    CALC_SPEED_NEG_INF,
    CALC_TIMING_UNCERTAINTY_ZERO,
    CALC_TIMING_UNCERTAINTY_NEGATIVE,
    CALC_TIMING_UNCERTAINTY_NAN,
    CALC_SPEED_UNCERTAINTY_NEGATIVE,
    CALC_SPEED_UNCERTAINTY_POS_INF,
    CALC_SPEED_UNCERTAINTY_ZERO_ALLOWED,
    CALC_SETTINGS_RATE_ZERO_LEGACY,
    CALC_GEOMETRY_COUNT_ZERO,
    CALC_GEOMETRY_COUNT_ONE,
    CALC_GEOMETRY_COUNT_TWO,
    CALC_GEOMETRY_COUNT_THREE,
    CALC_GEOMETRY_COUNT_FOUR,
    CALC_EMPTY_SESSION,
    CALC_ZERO_SESSION_RATE,
    CALC_ONE_SAMPLE_SESSION,
} calculate_case_kind_t;

typedef struct
{
    const char *name;
    const char *expected_reason;
    calculate_case_kind_t kind;
    esp_err_t expected_status;
} calculate_case_t;

static const calculate_case_t g_calculate_cases[] = {
    { "null object id", "ESP_ERR_INVALID_ARG_NULL_OBJECT_ID", CALC_NULL_OBJECT, ESP_ERR_INVALID_ARG },
    { "null session id", "ESP_ERR_INVALID_ARG_NULL_SESSION_ID", CALC_NULL_SESSION, ESP_ERR_INVALID_ARG },
    { "null calculate config", "ESP_ERR_INVALID_ARG_NULL_CONFIG", CALC_NULL_CONFIG, ESP_ERR_INVALID_ARG },
    { "null summary output", "ESP_ERR_INVALID_ARG_NULL_SUMMARY", CALC_NULL_SUMMARY, ESP_ERR_INVALID_ARG },
    { "calculate speed zero", "ESP_ERR_INVALID_ARG_SPEED_NOT_POSITIVE", CALC_SPEED_ZERO, ESP_ERR_INVALID_ARG },
    { "calculate speed negative", "ESP_ERR_INVALID_ARG_SPEED_NOT_POSITIVE", CALC_SPEED_NEGATIVE, ESP_ERR_INVALID_ARG },
    { "calculate speed NaN", "ESP_ERR_INVALID_ARG_SPEED_NONFINITE", CALC_SPEED_NAN, ESP_ERR_INVALID_ARG },
    { "calculate speed +Inf", "ESP_ERR_INVALID_ARG_SPEED_NONFINITE", CALC_SPEED_POS_INF, ESP_ERR_INVALID_ARG },
    { "calculate speed -Inf", "ESP_ERR_INVALID_ARG_SPEED_NONFINITE", CALC_SPEED_NEG_INF, ESP_ERR_INVALID_ARG },
    { "timing uncertainty zero", "ESP_ERR_INVALID_ARG_TIMING_UNCERTAINTY_NOT_POSITIVE", CALC_TIMING_UNCERTAINTY_ZERO, ESP_ERR_INVALID_ARG },
    { "timing uncertainty negative", "ESP_ERR_INVALID_ARG_TIMING_UNCERTAINTY_NOT_POSITIVE", CALC_TIMING_UNCERTAINTY_NEGATIVE, ESP_ERR_INVALID_ARG },
    { "timing uncertainty NaN", "ESP_ERR_INVALID_ARG_TIMING_UNCERTAINTY_NONFINITE", CALC_TIMING_UNCERTAINTY_NAN, ESP_ERR_INVALID_ARG },
    { "speed uncertainty negative", "ESP_ERR_INVALID_ARG_SPEED_UNCERTAINTY_NEGATIVE", CALC_SPEED_UNCERTAINTY_NEGATIVE, ESP_ERR_INVALID_ARG },
    { "speed uncertainty +Inf", "ESP_ERR_INVALID_ARG_SPEED_UNCERTAINTY_NONFINITE", CALC_SPEED_UNCERTAINTY_POS_INF, ESP_ERR_INVALID_ARG },
    { "zero speed uncertainty is allowed", "CONFIG_ACCEPTED_THEN_EMPTY_SESSION_INVALID_STATE", CALC_SPEED_UNCERTAINTY_ZERO_ALLOWED, ESP_ERR_INVALID_STATE },
    { "zero configured rate uses 500 Hz legacy default", "CONFIG_ACCEPTED_THEN_EMPTY_SESSION_INVALID_STATE", CALC_SETTINGS_RATE_ZERO_LEGACY, ESP_ERR_INVALID_STATE },
    { "calculate zero sensors", "ESP_ERR_INVALID_STATE_FEWER_THAN_3_PARTICIPANTS", CALC_GEOMETRY_COUNT_ZERO, ESP_ERR_INVALID_STATE },
    { "calculate one sensor", "ESP_ERR_INVALID_STATE_FEWER_THAN_3_PARTICIPANTS", CALC_GEOMETRY_COUNT_ONE, ESP_ERR_INVALID_STATE },
    { "calculate two sensors", "ESP_ERR_INVALID_STATE_FEWER_THAN_3_PARTICIPANTS", CALC_GEOMETRY_COUNT_TWO, ESP_ERR_INVALID_STATE },
    { "calculate three sensors planar threshold", "CALCULATE_ACCEPTS_3_AS_PLANAR_THEN_EMPTY_SESSION_REJECTS", CALC_GEOMETRY_COUNT_THREE, ESP_ERR_INVALID_STATE },
    { "calculate four sensors fixed-array maximum", "CALCULATE_ACCEPTS_4_AS_3D_CAPABLE_THEN_EMPTY_SESSION_REJECTS", CALC_GEOMETRY_COUNT_FOUR, ESP_ERR_INVALID_STATE },
    { "empty session waveform", "ESP_ERR_INVALID_STATE_NO_SAMPLES", CALC_EMPTY_SESSION, ESP_ERR_INVALID_STATE },
    { "zero session sample rate", "ESP_ERR_INVALID_STATE_SESSION_RATE_ZERO", CALC_ZERO_SESSION_RATE, ESP_ERR_INVALID_STATE },
    { "one-sample session waveform", "NO_SOLVED_SOURCE_FROM_ONE_SAMPLE", CALC_ONE_SAMPLE_SESSION, ESP_OK },
};

static void test_calculate_public_validation_matrix(void)
{
    for (size_t i = 0U; i < ARRAY_COUNT(g_calculate_cases); ++i) {
        const calculate_case_t *test = &g_calculate_cases[i];
        size_t fake_sensor_count = 4U;
        const size_t fake_sample_count = test->kind == CALC_ONE_SAMPLE_SESSION ? 1U : 0U;
        if (test->kind == CALC_GEOMETRY_COUNT_ZERO) fake_sensor_count = 0U;
        if (test->kind == CALC_GEOMETRY_COUNT_ONE) fake_sensor_count = 1U;
        if (test->kind == CALC_GEOMETRY_COUNT_TWO) fake_sensor_count = 2U;
        if (test->kind == CALC_GEOMETRY_COUNT_THREE) fake_sensor_count = 3U;
        reset_fakes(fake_sensor_count, fake_sample_count);
        localization_calculate_config_t config = valid_calculate_config();
        localization_calculate_config_t *config_arg = &config;
        localization_calculate_summary_t summary;
        memset(&summary, 0xA5, sizeof(summary));
        localization_calculate_summary_t *summary_arg = &summary;
        const char *object_id = "OBJ_0001";
        const char *session_id = "S000001";

        switch (test->kind) {
        case CALC_NULL_OBJECT: object_id = NULL; break;
        case CALC_NULL_SESSION: session_id = NULL; break;
        case CALC_NULL_CONFIG: config_arg = NULL; break;
        case CALC_NULL_SUMMARY: summary_arg = NULL; break;
        case CALC_SPEED_ZERO: config.propagation_speed_mps = 0.0; break;
        case CALC_SPEED_NEGATIVE: config.propagation_speed_mps = -1.0; break;
        case CALC_SPEED_NAN: config.propagation_speed_mps = NAN; break;
        case CALC_SPEED_POS_INF: config.propagation_speed_mps = INFINITY; break;
        case CALC_SPEED_NEG_INF: config.propagation_speed_mps = -INFINITY; break;
        case CALC_TIMING_UNCERTAINTY_ZERO: config.timing_uncertainty_us = 0.0; break;
        case CALC_TIMING_UNCERTAINTY_NEGATIVE: config.timing_uncertainty_us = -1.0; break;
        case CALC_TIMING_UNCERTAINTY_NAN: config.timing_uncertainty_us = NAN; break;
        case CALC_SPEED_UNCERTAINTY_NEGATIVE: config.propagation_speed_uncertainty_mps = -1.0; break;
        case CALC_SPEED_UNCERTAINTY_POS_INF: config.propagation_speed_uncertainty_mps = INFINITY; break;
        case CALC_SPEED_UNCERTAINTY_ZERO_ALLOWED: break;
        case CALC_SETTINGS_RATE_ZERO_LEGACY: config.sample_rate_hz = 0U; break;
        case CALC_GEOMETRY_COUNT_ZERO:
        case CALC_GEOMETRY_COUNT_ONE:
        case CALC_GEOMETRY_COUNT_TWO:
        case CALC_GEOMETRY_COUNT_THREE:
        case CALC_GEOMETRY_COUNT_FOUR:
            break;
        case CALC_EMPTY_SESSION: break;
        case CALC_ZERO_SESSION_RATE: g_fake_summary.sample_rate_hz = 0U; break;
        case CALC_ONE_SAMPLE_SESSION: g_fake_summary.samples_written = 1U; break;
        }

        const esp_err_t actual = localization_calculate_run(
            object_id, session_id, config_arg, 1U, summary_arg
        );
        assert(actual == test->expected_status);
        if (test->expected_status == ESP_ERR_INVALID_ARG) {
            assert(g_geometry_reads == 0U);
            assert(g_summary_reads == 0U);
            assert(g_run_writes == 0U);
        } else {
            if (g_geometry_reads != 1U) {
                fprintf(stderr, "case=%s geometry_reads=%u status=%d\n",
                    test->name, (unsigned)g_geometry_reads, actual);
            }
            assert(g_geometry_reads == 1U);
            const bool insufficient_geometry =
                test->kind == CALC_GEOMETRY_COUNT_ZERO ||
                test->kind == CALC_GEOMETRY_COUNT_ONE ||
                test->kind == CALC_GEOMETRY_COUNT_TWO;
            assert(g_summary_reads == (insufficient_geometry ? 0U : 1U));
        }
        if (test->kind == CALC_EMPTY_SESSION || test->kind == CALC_ZERO_SESSION_RATE) {
            assert(g_run_writes == 0U);
        }
        if (test->kind == CALC_ONE_SAMPLE_SESSION) {
            assert(g_range_reads > 0U);
            assert(summary.methods_solved == 0U);
        }
        const bool config_is_accepted_then_data_rejected =
            test->kind == CALC_SPEED_UNCERTAINTY_ZERO_ALLOWED ||
            test->kind == CALC_SETTINGS_RATE_ZERO_LEGACY ||
            test->kind == CALC_GEOMETRY_COUNT_THREE ||
            test->kind == CALC_GEOMETRY_COUNT_FOUR;
        printf("%s %s reason=%s status=%d\n",
            actual == ESP_OK || config_is_accepted_then_data_rejected ? "PASS" : "REJECT",
            test->name, test->expected_reason, actual);
    }
}

static localization_calculate_config_t valid_policy_config(uint32_t settings_rate_hz)
{
    localization_calculate_config_t config = valid_calculate_config();
    config.sample_rate_hz = settings_rate_hz;
    config.pick_config.noise_window_samples = 4U;
    config.pick_config.sta_window_samples = 2U;
    config.pick_config.lta_window_samples = 20U;
    config.pick_config.sta_lta_trigger_ratio = 3.0f;
    config.pick_config.refine_lookback_samples = 1U;
    config.pick_config.refine_lookahead_samples = 1U;
    config.pick_config.snr_signal_window_samples = 2U;
    config.semblance_window_half_span_samples = 1U;
    return config;
}

typedef enum
{
    RESCALE_NULL_CONFIG,
    RESCALE_ZERO_SESSION_RATE,
    RESCALE_ZERO_STA_LTA_WINDOWS,
    RESCALE_SIZE_OVERFLOW,
    RESCALE_MAX_RATE_BOUNDARY,
    RESCALE_LEGACY_ZERO_SETTINGS_RATE,
} rescale_case_kind_t;

typedef struct
{
    const char *name;
    const char *expected_reason;
    rescale_case_kind_t kind;
    esp_err_t expected_status;
} rescale_case_t;

static const rescale_case_t g_rescale_cases[] = {
    { "null policy config", "configuration must be present and session sample rate must be nonzero", RESCALE_NULL_CONFIG, ESP_ERR_INVALID_ARG },
    { "zero session sample rate", "configuration must be present and session sample rate must be nonzero", RESCALE_ZERO_SESSION_RATE, ESP_ERR_INVALID_ARG },
    { "zero STA/LTA windows after minimum-one scaling", "rescaled P-wave STA/LTA windows violate the trigger ratio rule", RESCALE_ZERO_STA_LTA_WINDOWS, ESP_ERR_INVALID_ARG },
    { "window count overflow", "sample-window rescaling overflowed size_t", RESCALE_SIZE_OVERFLOW, ESP_ERR_INVALID_SIZE },
    { "UINT32_MAX session rate boundary", "ACCEPT_VALID_RATE_BOUNDARY", RESCALE_MAX_RATE_BOUNDARY, ESP_OK },
    { "zero settings rate legacy convention", "ACCEPT_ZERO_SETTINGS_RATE_AS_500_HZ", RESCALE_LEGACY_ZERO_SETTINGS_RATE, ESP_OK },
};

static void test_rescale_validation_matrix(void)
{
    for (size_t i = 0U; i < ARRAY_COUNT(g_rescale_cases); ++i) {
        const rescale_case_t *test = &g_rescale_cases[i];
        localization_calculate_config_t config = valid_policy_config(500U);
        localization_calculate_config_t *config_arg = &config;
        uint32_t session_rate_hz = 500U;
        switch (test->kind) {
        case RESCALE_NULL_CONFIG: config_arg = NULL; break;
        case RESCALE_ZERO_SESSION_RATE: session_rate_hz = 0U; break;
        case RESCALE_ZERO_STA_LTA_WINDOWS:
            config.pick_config.sta_window_samples = 0U;
            config.pick_config.lta_window_samples = 1U;
            break;
        case RESCALE_SIZE_OVERFLOW:
            config.sample_rate_hz = 1U;
            config.pick_config.noise_window_samples = SIZE_MAX;
            session_rate_hz = UINT32_MAX;
            break;
        case RESCALE_MAX_RATE_BOUNDARY:
            config.sample_rate_hz = UINT32_MAX;
            session_rate_hz = UINT32_MAX;
            break;
        case RESCALE_LEGACY_ZERO_SETTINGS_RATE:
            config.sample_rate_hz = 0U;
            break;
        }

        char reason[96];
        reason[0] = '\0';
        const esp_err_t actual = localization_calculate_rescale_sample_windows(
            config_arg, session_rate_hz, reason, sizeof(reason)
        );
        assert(actual == test->expected_status);
        if (test->expected_status != ESP_OK) {
            assert(strcmp(reason, test->expected_reason) == 0);
        } else if (test->kind == RESCALE_LEGACY_ZERO_SETTINGS_RATE) {
            assert(config.sample_rate_hz == 500U);
        }
        printf("%s %s reason=%s status=%d\n",
            actual == ESP_OK ? "PASS" : "REJECT",
            test->name, test->expected_reason, actual);
    }
}

static void test_policy_scalar_and_unit_boundaries(void)
{
    struct
    {
        const char *name;
        double uncertainty_us;
    } uncertainty_cases[] = {
        { "zero timing uncertainty", 0.0 },
        { "negative timing uncertainty", -1.0 },
        { "NaN timing uncertainty", NAN },
        { "+Inf timing uncertainty", INFINITY },
        { "-Inf timing uncertainty", -INFINITY },
    };
    for (size_t i = 0U; i < ARRAY_COUNT(uncertainty_cases); ++i) {
        const double quality = localization_calculate_quality_from_residual_us(
            2.0, uncertainty_cases[i].uncertainty_us
        );
        assert(quality == 0.0);
        printf("REJECT %s reason=POLICY_QUALITY_ZERO_FOR_INVALID_UNCERTAINTY\n",
            uncertainty_cases[i].name);
    }

    object_store_pod_geometry_t pod;
    memset(&pod, 0, sizeof(pod));
    pod.computed = true;
    pod.x_mm = -1250;
    pod.y_mm = 2750;
    pod.z_mm = 325;
    const localization_point3d_t sensor = localization_calculate_sensor_position(&pod);
    assert(sensor.x_m == -1.25 && sensor.y_m == 2.75 && sensor.z_m == 0.325);
    assert(localization_calculate_arrival_time_us(25U, 25000U) == 1000);
    assert(localization_calculate_arrival_time_us(7U, 1000U) == 7000);
    assert(localization_calculate_arrival_time_us(7U, 0U) == 0);

    const localization_point3d_t planar_positions[] = {
        { 0.0, 0.0, 1.0 }, { 2.0, 0.0, 1.0 },
    };
    localization_semblance_search_bounds_t semblance_bounds;
    localization_traveltime_search_bounds_t traveltime_bounds;
    localization_calculate_search_bounds(
        planar_positions, ARRAY_COUNT(planar_positions),
        LOCALIZATION_DIMENSIONALITY_PLANAR_2D, 0.0,
        &semblance_bounds, &traveltime_bounds
    );
    /* A 2 m sensor span makes the half-diagonal margin exactly 1 m.  Both
     * horizontal faces are included by their exact returned coordinates;
     * planar Z is a single equal-valued layer at mean sensor elevation. */
    assert(semblance_bounds.min_corner.x_m == -1.0);
    assert(semblance_bounds.max_corner.x_m == 3.0);
    assert(semblance_bounds.min_corner.y_m == -1.0);
    assert(semblance_bounds.max_corner.y_m == 1.0);
    assert(semblance_bounds.min_corner.z_m == 1.0);
    assert(semblance_bounds.max_corner.z_m == 1.0);
    assert(traveltime_bounds.min_corner.z_m == traveltime_bounds.max_corner.z_m);
    puts("PASS policy units: mm-to-m, sample-to-us, and inclusive planar search faces");
}

static void test_event_window_rejects_bad_inputs(void)
{
    struct
    {
        const char *name;
        double speed_mps;
        double max_distance_m;
        uint32_t sample_rate_hz;
        size_t reference_index;
    } cases[] = {
        { "zero event-window speed", 0.0, 2.0, 500U, 10U },
        { "negative event-window speed", -1.0, 2.0, 500U, 10U },
        { "NaN event-window distance", 340.0, NAN, 500U, 10U },
        { "zero event-window sample rate", 340.0, 2.0, 0U, 10U },
        { "reference at frame-count boundary", 340.0, 2.0, 500U, 100U },
    };
    for (size_t i = 0U; i < ARRAY_COUNT(cases); ++i) {
        size_t start = 777U;
        size_t end = 888U;
        assert(!localization_calculate_event_window(
            cases[i].reference_index, 100U, 20U, cases[i].max_distance_m,
            cases[i].speed_mps, cases[i].sample_rate_hz, &start, &end
        ));
        assert(start == 777U && end == 888U);
        printf("REJECT %s reason=POLICY_EVENT_WINDOW_FALSE_INVALID_INPUT\n", cases[i].name);
    }
}

static void test_nonrepresentable_cases_are_explicitly_not_applicable(void)
{
    static const struct
    {
        const char *name;
        const char *reason;
    } cases[] = {
        { "duplicate sensor IDs", "TDOA observation has no ID field" },
        { "NaN or infinite arrival time", "arrival_time_us is int64_t, so nonfinite values cannot be passed" },
        { "negative sample rate", "public sample-rate fields are uint32_t" },
        { "mismatched channel lengths", "calculate consumes fixed-width seismic_frame_t records, not per-channel arrays" },
    };
    for (size_t i = 0U; i < ARRAY_COUNT(cases); ++i) {
        printf("N/A %s reason=NOT_ACCEPTED_BY_TESTED_ENTRY_POINT detail=%s\n",
            cases[i].name, cases[i].reason);
    }
}

int main(void)
{
    test_calculate_public_validation_matrix();
    test_rescale_validation_matrix();
    test_policy_scalar_and_unit_boundaries();
    test_event_window_rejects_bad_inputs();
    test_nonrepresentable_cases_are_explicitly_not_applicable();
    puts("MAX-132 calculate/policy validation tests passed");
    return 0;
}
