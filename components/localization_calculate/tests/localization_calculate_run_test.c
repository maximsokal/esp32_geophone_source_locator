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
#include "localization_calculate_policy.h"
#include "localization_dimensionality_policy.h"
#include "localization_run_store.h"
#include "object_store.h"
#include "scan_occupancy.h"
#include "seismic_recorder.h"
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
static esp_err_t g_fixture_geometry_err;
static esp_err_t g_fixture_raw_err;
static size_t g_geometry_read_calls;
static size_t g_raw_read_calls;

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

esp_err_t seismic_recorder_read_session_raw(
    const char *object_id,
    const char *session_id,
    seismic_frame_t *frames,
    size_t frame_capacity,
    size_t *frame_count_out,
    seismic_recorder_session_summary_t *summary_out
)
{
    (void)object_id;
    (void)session_id;
    ++g_raw_read_calls;
    if (g_fixture_raw_err != ESP_OK) {
        return g_fixture_raw_err;
    }
    if (frame_capacity < g_fixture_frame_count) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(frames, g_fixture_frames, sizeof(*frames) * g_fixture_frame_count);
    *frame_count_out = g_fixture_frame_count;
    if (summary_out != NULL) {
        *summary_out = g_fixture_summary;
    }
    return ESP_OK;
}


/* ==== Fake localization_run_store: records every write ==================== */

#define MAX_RECORDED_WRITES 32U

static localization_run_record_t g_recorded[MAX_RECORDED_WRITES];
static size_t g_recorded_count;
static size_t g_write_call_count;
static size_t g_pick_write_calls;

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
    (void)record;
    ++g_pick_write_calls;
    return ESP_OK;
}


static void reset_fakes(void)
{
    g_now_us = 1000000;
    memset(&g_fixture_geometry, 0, sizeof(g_fixture_geometry));
    g_fixture_geometry_err = ESP_OK;
    g_fixture_raw_err = ESP_OK;
    g_geometry_read_calls = 0U;
    g_raw_read_calls = 0U;

    g_recorded_count = 0U;
    g_write_call_count = 0U;
    g_pick_write_calls = 0U;
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
    g_fixture_summary.sample_rate_hz = FIX_SAMPLE_RATE_HZ;
    g_fixture_summary.channel_count = OBJECT_STORE_POD_COUNT;
}


static localization_calculate_config_t default_test_config(void)
{
    localization_calculate_config_t config;
    memset(&config, 0, sizeof(config));
    config.propagation_speed_mps = 340.0;
    config.propagation_speed_uncertainty_mps = 20.0;
    snprintf(config.propagation_model_name, sizeof(config.propagation_model_name), "%s", "test-model");
    config.timing_uncertainty_us = 60.0;
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


static const localization_point3d_t s_source = { 0.25, 0.40, 0.30 };
#define TEST_SPEED_MPS 340.0
#define TEST_EMISSION_S 0.01


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
        assert(g_raw_read_calls == 0U); /* geometry gate fails before raw is ever read */
        assert(g_write_call_count == 0U);
        assert(g_pick_write_calls == 0U);
        assert(g_recorded_count == 0U);
    }
    free_fixture_frames();
    puts("  insufficient dimensionality (0/1/2 pods): zero run-store writes -- OK");
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
    localization_calculate_summary_t summary;
    const esp_err_t err = localization_calculate_run(
        "OBJ01", "S000003", &config, 44000ULL, &summary
    );

    assert(err == ESP_OK);
    assert(summary.participating_channel_count == 4U);
    assert(g_recorded_count == 13U);

    for (size_t i = 0U; i < g_recorded_count; ++i) {
        assert(g_recorded[i].dimensionality == LOCALIZATION_DIMENSIONALITY_CAPABLE_3D);
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

    for (size_t i = 0U; i < g_recorded_count; ++i) {
        assert(!g_recorded[i].solved);
        assert(strlen(g_recorded[i].rejection_reason) > 0U);
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
    assert(g_raw_read_calls == 1U);
    assert(g_write_call_count == 0U);
    assert(g_pick_write_calls == 0U);
    assert(g_recorded_count == 0U);

    reset_fakes();
    build_fixture(4U, &s_source, TEST_SPEED_MPS, TEST_EMISSION_S, 1.0f, 0.02f, true);
    g_fixture_geometry_err = ESP_FAIL;

    err = localization_calculate_run("OBJ01", "S000006", &config, 47000ULL, &summary);

    assert(err == ESP_FAIL);
    assert(g_geometry_read_calls == 1U);
    assert(g_raw_read_calls == 0U); /* geometry is read and checked before raw samples at all */
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
    }

    const localization_run_record_t *tdoa = find_record("tdoa_gauss_newton");
    const localization_run_record_t *energy = find_record("energy_baseline");
    assert(tdoa != NULL && energy != NULL);
    assert(strcmp(tdoa->rejection_reason, LOCALIZATION_CALCULATE_TIME_BUDGET_REASON) != 0);
    assert(strcmp(energy->rejection_reason, LOCALIZATION_CALCULATE_TIME_BUDGET_REASON) != 0
           || energy->solved);

    puts("  time budget exhausted: 10 skippable methods report the budget reason -- OK");
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

    static const char *const not_applied_reason =
        "scan occupancy artifact unreadable or invalid: constraint not applied";
    for (size_t i = 0U; i < sizeof(scan_methods) / sizeof(scan_methods[0]); ++i) {
        const localization_run_record_t *record = find_record(scan_methods[i]);
        assert(record != NULL);
        assert(!record->solved);
        assert(strcmp(record->rejection_reason, not_applied_reason) == 0);
    }

    remove(occupancy_path);
    puts("  unusable scan artifact: 8 scan variants report 'constraint not applied' -- OK");
}


int main(void)
{
    test_insufficient_dimensionality_writes_nothing();
    test_three_pods_planar_no_fabricated_z();
    test_four_pods_clean_event_solves_near_truth();
    test_flat_signal_every_method_unsolved_with_reason();
    test_read_failures_propagate_with_no_partial_writes();
    test_run_store_write_failure_midway_contract();
    test_time_budget_exhausted_skips_remaining_methods();
    test_unusable_scan_reports_constraint_not_applied();

    free_fixture_frames();
    puts("localization_calculate_run integration tests passed");
    return 0;
}
