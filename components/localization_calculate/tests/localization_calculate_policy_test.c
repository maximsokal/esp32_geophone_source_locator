#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "localization_calculate_policy.h"


static bool nearly_equal(double a, double b, double epsilon)
{
    return fabs(a - b) < epsilon;
}


static void test_quality_from_residual_matches_the_gaussian_formula(void)
{
    /* exp(-0.5 * (10/10)^2) = exp(-0.5) */
    assert(nearly_equal(
        localization_calculate_quality_from_residual_us(10.0, 10.0),
        exp(-0.5), 1e-9
    ));
    /* A perfect (zero-residual) fit scores exactly 1.0. */
    assert(localization_calculate_quality_from_residual_us(0.0, 10.0) == 1.0);
}


static void test_quality_from_residual_fails_closed_not_nan(void)
{
    assert(localization_calculate_quality_from_residual_us(10.0, 0.0) == 0.0);
    assert(localization_calculate_quality_from_residual_us(10.0, -1.0) == 0.0);
    assert(localization_calculate_quality_from_residual_us(NAN, 10.0) == 0.0);
    assert(localization_calculate_quality_from_residual_us(INFINITY, 10.0) == 0.0);
    assert(localization_calculate_quality_from_residual_us(10.0, NAN) == 0.0);
}


static object_store_geometry_t geometry_with_pods(
    bool p0, bool p1, bool p2, bool p3
)
{
    object_store_geometry_t geometry;
    memset(&geometry, 0, sizeof(geometry));
    geometry.pods[0].computed = p0;
    geometry.pods[1].computed = p1;
    geometry.pods[2].computed = p2;
    geometry.pods[3].computed = p3;
    geometry.pods[0].x_mm = 300;
    geometry.pods[1].x_mm = -150;
    geometry.pods[1].y_mm = 200;
    geometry.pods[2].y_mm = -200;
    geometry.pods[3].z_mm = 50;
    return geometry;
}


static void test_participating_channels_counts_and_masks_computed_pods(void)
{
    const object_store_geometry_t geometry = geometry_with_pods(true, false, true, true);
    uint32_t mask = 0xFFFFFFFFU;
    const size_t count = localization_calculate_participating_channels(&geometry, &mask);
    assert(count == 3U);
    /* bits 0, 2, 3 set; bit 1 clear. */
    assert(mask == ((1U << 0) | (1U << 2) | (1U << 3)));
}


static void test_participating_channels_handles_null_geometry(void)
{
    uint32_t mask = 0xFFFFFFFFU;
    assert(localization_calculate_participating_channels(NULL, &mask) == 0U);
    assert(mask == 0U);
    /* mask_out is optional. */
    assert(localization_calculate_participating_channels(NULL, NULL) == 0U);
}


static void test_sensor_position_converts_millimeters_to_meters(void)
{
    object_store_pod_geometry_t pod;
    memset(&pod, 0, sizeof(pod));
    pod.x_mm = 3200;
    pod.y_mm = -450;
    pod.z_mm = 10;

    const localization_point3d_t position = localization_calculate_sensor_position(&pod);
    assert(position.x_m == 3.2);
    assert(position.y_m == -0.45);
    assert(position.z_m == 0.01);

    const localization_point3d_t null_position = localization_calculate_sensor_position(NULL);
    assert(null_position.x_m == 0.0 && null_position.y_m == 0.0 && null_position.z_m == 0.0);
}


static void test_search_bounds_wraps_the_sensor_bounding_box_with_a_margin(void)
{
    const localization_point3d_t positions[3] = {
        { 0.0, 0.0, 0.0 },
        { 3.0, 0.0, 0.0 },
        { 1.5, 2.0, 0.5 },
    };
    localization_semblance_search_bounds_t semblance_bounds;
    localization_traveltime_search_bounds_t traveltime_bounds;

    localization_calculate_search_bounds(
        positions, 3U, LOCALIZATION_DIMENSIONALITY_CAPABLE_3D, 0.0,
        &semblance_bounds, &traveltime_bounds
    );

    /* The box must fully contain every sensor, with room to spare. */
    assert(semblance_bounds.min_corner.x_m < 0.0);
    assert(semblance_bounds.max_corner.x_m > 3.0);
    assert(semblance_bounds.min_corner.y_m < 0.0);
    assert(semblance_bounds.max_corner.y_m > 2.0);
    assert(semblance_bounds.step_m > 0.0);

    /* Both backends get the exact same box/resolution. */
    assert(traveltime_bounds.min_corner.x_m == semblance_bounds.min_corner.x_m);
    assert(traveltime_bounds.max_corner.y_m == semblance_bounds.max_corner.y_m);
    assert(traveltime_bounds.step_m == semblance_bounds.step_m);
}


static void test_search_bounds_collapses_to_one_layer_when_planar(void)
{
    const localization_point3d_t positions[3] = {
        { 0.0, 0.0, 0.1 },
        { 3.0, 0.0, 0.3 },
        { 1.5, 2.0, 0.2 },
    };
    localization_semblance_search_bounds_t semblance_bounds;
    localization_traveltime_search_bounds_t traveltime_bounds;

    localization_calculate_search_bounds(
        positions, 3U, LOCALIZATION_DIMENSIONALITY_PLANAR_2D, 0.0,
        &semblance_bounds, &traveltime_bounds
    );

    assert(semblance_bounds.min_corner.z_m == semblance_bounds.max_corner.z_m);
    assert(traveltime_bounds.min_corner.z_m == traveltime_bounds.max_corner.z_m);
}


static void test_search_bounds_handles_zero_positions_without_crashing(void)
{
    localization_semblance_search_bounds_t semblance_bounds;
    localization_traveltime_search_bounds_t traveltime_bounds;
    localization_calculate_search_bounds(
        NULL, 0U, LOCALIZATION_DIMENSIONALITY_CAPABLE_3D, 0.0,
        &semblance_bounds, &traveltime_bounds
    );
    assert(semblance_bounds.max_corner.x_m > semblance_bounds.min_corner.x_m);
}


/* MAX-211: a buried source is only searched for below the surface the
 * geophones lie on, never in its mirror position above it, and an explicit
 * maximum depth is honored. */
static void test_search_bounds_stay_below_surface_and_honor_depth(void)
{
    const localization_point3d_t positions[4] = {
        { 0.0, 0.0, 0.10 },
        { 5.0, 0.0, 0.30 },
        { 0.0, 5.0, 0.00 },
        { 5.0, 5.0, 0.20 },
    };
    localization_semblance_search_bounds_t semblance_bounds;
    localization_traveltime_search_bounds_t traveltime_bounds;

    localization_calculate_search_bounds(
        positions, 4U, LOCALIZATION_DIMENSIONALITY_CAPABLE_3D, 0.0,
        &semblance_bounds, &traveltime_bounds
    );
    /* Top of the box is the highest sensor, not above it. */
    assert(semblance_bounds.max_corner.z_m == 0.30);
    assert(traveltime_bounds.max_corner.z_m == 0.30);
    /* Default depth: half the 5x5 m diagonal below the lowest sensor. */
    assert(fabs(semblance_bounds.min_corner.z_m - (0.0 - sqrt(50.0) / 2.0)) < 1e-9);

    localization_calculate_search_bounds(
        positions, 4U, LOCALIZATION_DIMENSIONALITY_CAPABLE_3D, 8.0,
        &semblance_bounds, &traveltime_bounds
    );
    assert(semblance_bounds.min_corner.z_m == -8.0);
    assert(traveltime_bounds.min_corner.z_m == -8.0);
    assert(traveltime_bounds.max_corner.z_m == 0.30);
}


static void test_geometry_computed_at_ms_takes_the_latest_computed_pod(void)
{
    object_store_geometry_t geometry = geometry_with_pods(true, false, true, true);
    geometry.pods[0].computed_at_ms = 1000ULL;
    geometry.pods[2].computed_at_ms = 3000ULL;
    geometry.pods[3].computed_at_ms = 2000ULL;
    /* Pod 1 is not computed; its timestamp must never win even if larger. */
    geometry.pods[1].computed_at_ms = 9999ULL;

    assert(localization_calculate_geometry_computed_at_ms(&geometry) == 3000ULL);
    assert(localization_calculate_geometry_computed_at_ms(NULL) == 0ULL);

    object_store_geometry_t none_computed;
    memset(&none_computed, 0, sizeof(none_computed));
    assert(localization_calculate_geometry_computed_at_ms(&none_computed) == 0ULL);
}


static void test_arrival_time_us_converts_sample_index_precisely(void)
{
    /* 250 samples at 500 Hz = 0.5 s = 500000 us. */
    assert(localization_calculate_arrival_time_us(250U, 500U) == 500000);
    assert(localization_calculate_arrival_time_us(0U, 500U) == 0);
    /* Never divides by zero -- fails closed to 0 instead. */
    assert(localization_calculate_arrival_time_us(250U, 0U) == 0);
}


static localization_calculate_run_context_t make_context(void)
{
    localization_calculate_run_context_t context;
    memset(&context, 0, sizeof(context));
    context.object_id = "OBJ_0001";
    context.session_id = "S000001";
    context.computed_at_ms = 123456ULL;
    context.geometry_computed_at_ms = 111ULL;
    context.dimensionality = LOCALIZATION_DIMENSIONALITY_CAPABLE_3D;
    context.propagation_model_name = "tap_calibrated";
    context.propagation_speed_mps = 340.0;
    context.propagation_speed_uncertainty_mps = 5.0;
    return context;
}


static void test_traveltime_adapter_common_fields(void)
{
    const localization_calculate_run_context_t context = make_context();
    localization_traveltime_result_t result;
    memset(&result, 0, sizeof(result));
    result.status = LOCALIZATION_TRAVELTIME_SOLVED;
    result.best_position = (localization_point3d_t) { 1.0, 2.0, 3.0 };
    result.best_residual_rms_us = 5.0;

    localization_run_record_t record;
    localization_calculate_traveltime_to_run_record(
        &context, &result, 0x07U, 0x08U, 1U, 10.0, &record
    );

    assert(record.schema_version == LOCALIZATION_RUN_SCHEMA_VERSION);
    assert(strcmp(record.object_id, "OBJ_0001") == 0);
    assert(strcmp(record.session_id, "S000001") == 0);
    assert(record.computed_at_ms == 123456ULL);
    assert(record.kind == LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD);
    assert(strcmp(record.method_name, "traveltime_inversion") == 0);
    assert(record.channel_mask_used == 0x07U);
    assert(record.channel_mask_rejected == 0x08U);
    assert(record.dimensionality == LOCALIZATION_DIMENSIONALITY_CAPABLE_3D);
    assert(record.geometry_computed_at_ms == 111ULL);
    assert(strcmp(record.propagation_model_name, "tap_calibrated") == 0);
    assert(record.propagation_speed_mps == 340.0);

    assert(record.solved);
    assert(record.source_position.x_m == 1.0);
    assert(record.rms_residual_us == 5.0);
    assert(nearly_equal(
        record.confidence_score,
        localization_calculate_quality_from_residual_us(5.0, 10.0),
        1e-12
    ));
}


static void test_traveltime_adapter_insufficient_picks_is_unsolved_with_reason(void)
{
    const localization_calculate_run_context_t context = make_context();
    localization_traveltime_result_t result;
    memset(&result, 0, sizeof(result));
    result.status = LOCALIZATION_TRAVELTIME_INSUFFICIENT_PICKS;

    localization_run_record_t record;
    localization_calculate_traveltime_to_run_record(
        &context, &result, 0U, 0x0FU, 1U, 10.0, &record
    );

    assert(!record.solved);
    assert(record.rejection_reason[0] != '\0');
}


static void test_traveltime_adapter_null_result_is_unsolved(void)
{
    const localization_calculate_run_context_t context = make_context();
    localization_run_record_t record;
    localization_calculate_traveltime_to_run_record(
        &context, NULL, 0U, 0x0FU, 1U, 10.0, &record
    );
    assert(!record.solved);
    assert(record.rejection_reason[0] != '\0');
}


static void test_semblance_adapter_not_attempted_uses_the_given_reason(void)
{
    const localization_calculate_run_context_t context = make_context();
    localization_run_record_t record;
    localization_calculate_semblance_to_run_record(
        &context, NULL, "no accepted arrival pick to anchor the search window",
        0U, 0x0FU, 1U, &record
    );
    assert(strcmp(record.method_name, "semblance") == 0);
    assert(!record.solved);
    assert(strcmp(record.rejection_reason, "no accepted arrival pick to anchor the search window") == 0);
}


static void test_semblance_adapter_found_is_solved(void)
{
    const localization_calculate_run_context_t context = make_context();
    localization_semblance_result_t result;
    memset(&result, 0, sizeof(result));
    result.found = true;
    result.best_position = (localization_point3d_t) { 0.5, 0.5, 0.0 };
    result.best_semblance = 0.87;

    localization_run_record_t record;
    localization_calculate_semblance_to_run_record(
        &context, &result, NULL, 0x0FU, 0U, 1U, &record
    );
    assert(record.solved);
    assert(record.source_position.y_m == 0.5);
    assert(record.confidence_score == 0.87);
}


static void test_semblance_adapter_not_found_is_unsolved(void)
{
    const localization_calculate_run_context_t context = make_context();
    localization_semblance_result_t result;
    memset(&result, 0, sizeof(result));
    result.found = false;

    localization_run_record_t record;
    localization_calculate_semblance_to_run_record(
        &context, &result, NULL, 0x0FU, 0U, 1U, &record
    );
    assert(!record.solved);
    assert(record.rejection_reason[0] != '\0');
}


static void test_energy_baseline_adapter(void)
{
    const localization_calculate_run_context_t context = make_context();
    localization_energy_baseline_result_t solved;
    memset(&solved, 0, sizeof(solved));
    solved.found = true;
    solved.position = (localization_point3d_t) { 2.0, -1.0, 0.2 };
    solved.energy_concentration = 0.6;

    localization_run_record_t record;
    localization_calculate_energy_baseline_to_run_record(&context, &solved, 0x0FU, 0U, 1U, &record);
    assert(strcmp(record.method_name, "energy_baseline") == 0);
    assert(record.solved);
    assert(record.confidence_score == 0.6);

    localization_energy_baseline_result_t unsolved;
    memset(&unsolved, 0, sizeof(unsolved));
    unsolved.found = false;
    localization_calculate_energy_baseline_to_run_record(&context, &unsolved, 0x0FU, 0U, 1U, &record);
    assert(!record.solved);
    assert(record.rejection_reason[0] != '\0');

    localization_calculate_energy_baseline_to_run_record(&context, NULL, 0x0FU, 0U, 1U, &record);
    assert(!record.solved);
}


static void test_joint_adapter_averages_bounded_quality(void)
{
    const localization_calculate_run_context_t context = make_context();
    localization_comparison_result_t result;
    memset(&result, 0, sizeof(result));
    result.joint_found = true;
    result.joint_position = (localization_point3d_t) { 1.0, 1.0, 1.0 };
    /* Two methods each near-perfect quality: sum could exceed 1, the
     * persisted confidence_score must not. */
    result.joint_total_quality = 1.8;
    result.joint_methods_used_count = 2U;

    localization_run_record_t record;
    localization_calculate_joint_to_run_record(&context, &result, 0x0FU, 0U, 1U, &record);

    assert(record.kind == LOCALIZATION_RUN_KIND_JOINT_FUSION);
    assert(strcmp(record.method_name, "joint_fusion") == 0);
    assert(record.solved);
    assert(record.confidence_score == 0.9);
    assert(record.confidence_score >= 0.0 && record.confidence_score <= 1.0);
}


static void test_joint_adapter_not_found_is_unsolved(void)
{
    const localization_calculate_run_context_t context = make_context();
    localization_comparison_result_t result;
    memset(&result, 0, sizeof(result));
    result.joint_found = false;

    localization_run_record_t record;
    localization_calculate_joint_to_run_record(&context, &result, 0U, 0x0FU, 1U, &record);
    assert(!record.solved);
    assert(record.rejection_reason[0] != '\0');

    localization_calculate_joint_to_run_record(&context, NULL, 0U, 0x0FU, 1U, &record);
    assert(!record.solved);
}


static void test_run_record_to_comparison_method_round_trips_solved(void)
{
    localization_run_record_t record;
    memset(&record, 0, sizeof(record));
    snprintf(record.method_name, sizeof(record.method_name), "semblance");
    record.solved = true;
    record.source_position = (localization_point3d_t) { 4.0, 5.0, 6.0 };
    record.confidence_score = 0.42;

    localization_comparison_method_result_t method;
    localization_calculate_run_record_to_comparison_method(&record, &method);

    assert(strcmp(method.method_name, "semblance") == 0);
    assert(method.solved);
    assert(method.position.x_m == 4.0);
    assert(method.quality == 0.42);
}


static void test_run_record_to_comparison_method_round_trips_unsolved(void)
{
    localization_run_record_t record;
    memset(&record, 0, sizeof(record));
    snprintf(record.method_name, sizeof(record.method_name), "traveltime_inversion");
    record.solved = false;
    snprintf(record.rejection_reason, sizeof(record.rejection_reason), "insufficient picks");

    localization_comparison_method_result_t method;
    localization_calculate_run_record_to_comparison_method(&record, &method);

    assert(!method.solved);
    assert(strcmp(method.rejection_reason, "insufficient picks") == 0);
}



/* MAX-212: with 4 sensors a 3D differential fit has no redundancy; a
 * perfect-looking residual must not read as a confirmed position. */
static void test_redundancy_caps_unverified_confidence(void)
{
    assert(localization_calculate_degrees_of_freedom(4U, false) == 0);
    assert(localization_calculate_degrees_of_freedom(3U, true) == 0);
    assert(localization_calculate_degrees_of_freedom(5U, false) == 1);
    assert(localization_calculate_degrees_of_freedom(1U, false) == -1);

    localization_run_record_t record;
    memset(&record, 0, sizeof(record));
    record.solved = true;
    record.confidence_score = localization_calculate_quality_from_residual_us(0.0, 2000.0);
    assert(record.confidence_score == 1.0);
    localization_calculate_apply_redundancy(&record, 0);
    assert(record.degrees_of_freedom == 0);
    assert(record.confidence_score == LOCALIZATION_CALCULATE_UNVERIFIED_CONFIDENCE_CAP);

    record.confidence_score = 0.9;
    localization_calculate_apply_redundancy(&record, 2);
    assert(record.confidence_score == 0.9);
    assert(record.degrees_of_freedom == 2);
}


static void test_event_window_covers_signal_and_moveout(void)
{
    size_t start = 0U;
    size_t end = 0U;
    /* 7.07 m at 340 m/s and 500 Hz: ceil(10.4) = 11 samples of moveout. */
    assert(localization_calculate_event_window(1000U, 30000U, 50U, 7.07, 340.0, 500U, &start, &end));
    assert(start == 1000U);
    assert(end == 1000U + 50U + 11U + 1U);

    /* Clamped at the end of the recording. */
    assert(localization_calculate_event_window(29990U, 30000U, 50U, 7.07, 340.0, 500U, &start, &end));
    assert(end == 30000U);

    assert(!localization_calculate_event_window(30000U, 30000U, 50U, 7.07, 340.0, 500U, &start, &end));
    assert(!localization_calculate_event_window(10U, 30000U, 50U, 7.07, 0.0, 500U, &start, &end));
}


static void test_fusion_mapping_groups_picks_and_excludes_energy(void)
{
    localization_run_record_t record;
    memset(&record, 0, sizeof(record));
    record.solved = true;
    record.confidence_score = 0.4;
    localization_comparison_method_result_t method;

    snprintf(record.method_name, sizeof(record.method_name), "%s", "tdoa_gauss_newton");
    localization_calculate_run_record_to_comparison_method(&record, &method);
    assert(method.evidence_group == LOCALIZATION_CALCULATE_PICK_EVIDENCE_GROUP);
    assert(method.quality == 0.4);

    snprintf(record.method_name, sizeof(record.method_name), "%s", "traveltime_inversion");
    localization_calculate_run_record_to_comparison_method(&record, &method);
    assert(method.evidence_group == LOCALIZATION_CALCULATE_PICK_EVIDENCE_GROUP);

    snprintf(record.method_name, sizeof(record.method_name), "%s", "semblance");
    localization_calculate_run_record_to_comparison_method(&record, &method);
    assert(method.evidence_group == 0U);
    assert(method.quality == 0.4);

    /* Energy centroid is diagnostic only: zero fusion weight, still solved. */
    snprintf(record.method_name, sizeof(record.method_name), "%s", "energy_baseline");
    localization_calculate_run_record_to_comparison_method(&record, &method);
    assert(method.solved);
    assert(method.quality == 0.0);
}


/* MAX-209: four sensors 5 m apart at 300 m/s allow at most ~23.6 ms of
 * arrival difference across the diagonal. Channel 3 "arrived" 0.5 s after
 * the others (a footstep, not the event) and must be excluded. */
static void test_consistency_gate_rejects_physically_impossible_pick(void)
{
    const localization_point3d_t positions[4] = {
        { 0.0, 0.0, 0.0 }, { 5.0, 0.0, 0.0 }, { 0.0, 5.0, 0.0 }, { 5.0, 5.0, 0.0 },
    };
    double arrival_s[4] = { 1.000, 1.010, 1.012, 1.520 };
    assert(localization_calculate_consistent_pick_mask(
        arrival_s, positions, 4U, 0xFU, 300.0, 0.0, 0.002
    ) == 0x7U);

    /* All plausible: nothing is rejected. */
    arrival_s[3] = 1.018;
    assert(localization_calculate_consistent_pick_mask(
        arrival_s, positions, 4U, 0xFU, 300.0, 0.0, 0.002
    ) == 0xFU);

    /* Only accepted candidates are considered. */
    assert(localization_calculate_consistent_pick_mask(
        arrival_s, positions, 4U, 0x5U, 300.0, 0.0, 0.002
    ) == 0x5U);

    /* A slower worst-case speed widens the window: 5 m at 150 m/s = 33 ms. */
    arrival_s[1] = 1.030;
    assert(localization_calculate_consistent_pick_mask(
        arrival_s, positions, 4U, 0x3U, 300.0, 0.0, 0.002
    ) == 0x1U);
    assert(localization_calculate_consistent_pick_mask(
        arrival_s, positions, 4U, 0x3U, 300.0, 150.0, 0.002
    ) == 0x3U);
}


static void test_xcorr_refines_relative_delay_to_sub_sample(void)
{
    float reference[600];
    float channel[600];
    for (size_t i = 0U; i < 600U; ++i) {
        const double ta = ((double)i - 300.0) / 5.0;
        const double tb = ((double)i - 307.3) / 5.0;
        reference[i] = (float)exp(-0.5 * ta * ta);
        channel[i] = (float)(0.5 * exp(-0.5 * tb * tb));
    }
    double delay = 0.0;
    /* Picks said 7 samples; xcorr refines to ~7.3. */
    assert(localization_calculate_xcorr_relative_delay(
        reference, channel, 600U, 290U, 7.0, 60U, 20U, 2.0, &delay
    ));
    assert(fabs(delay - 7.3) < 0.1);

    /* A pick difference far from the correlation peak is not overridden. */
    assert(!localization_calculate_xcorr_relative_delay(
        reference, channel, 600U, 290U, 15.0, 60U, 20U, 2.0, &delay
    ));

    /* Dissimilar windows (noise vs pulse) are not trusted. */
    for (size_t i = 0U; i < 600U; ++i) {
        channel[i] = (float)((i * 7919U) % 13U) - 6.0f;
    }
    assert(!localization_calculate_xcorr_relative_delay(
        reference, channel, 600U, 290U, 0.0, 60U, 20U, 20.0, &delay
    ));
}


static void test_fractional_arrival_time(void)
{
    geophone_channel_pick_t pick;
    memset(&pick, 0, sizeof(pick));
    pick.arrival_sample_index = 1000U;
    pick.arrival_subsample_offset = -0.25f;
    assert(localization_calculate_pick_sample_position(&pick) == 999.75);
    assert(localization_calculate_arrival_time_us_at(999.75, 500U) == 1999500);
}


/* MAX-147: scan-constrained admission. Occupancy (voxel 1 m): FREE cell
 * (2,0,0), OCCUPIED cell (0,0,0); envelope [-1,-1,-1]..[4,4,1] with 2 m of
 * candidate volume below it. Registration shifts sensor x by +1 m. */
static int64_t test_pack_key(int32_t ix, int32_t iy, int32_t iz)
{
    return ((int64_t)(ix + 32768) << 32) | ((int64_t)(iy + 32768) << 16) | (int64_t)(iz + 32768);
}

static void test_scan_candidate_filter_rejects_free_and_outside(void)
{
    int64_t free_keys[1] = { test_pack_key(2, 0, 0) };
    int64_t occupied_keys[1] = { test_pack_key(0, 0, 0) };
    scan_occupancy_t occupancy;
    memset(&occupancy, 0, sizeof(occupancy));
    occupancy.voxel_size_m = 1.0;
    occupancy.below_surface_m = 2.0;
    occupancy.has_envelope = true;
    occupancy.envelope_min[0] = -1.0; occupancy.envelope_min[1] = -1.0; occupancy.envelope_min[2] = -1.0;
    occupancy.envelope_max[0] = 4.0; occupancy.envelope_max[1] = 4.0; occupancy.envelope_max[2] = 1.0;
    occupancy.free_keys = free_keys;
    occupancy.free_count = 1U;
    occupancy.occupied_keys = occupied_keys;
    occupancy.occupied_count = 1U;
    occupancy.registration.present = true;
    occupancy.registration.geometry_computed_at_ms = 42U;
    const double identity_shift[16] = { 1, 0, 0, 1,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
    memcpy(occupancy.registration.sensor_to_scan, identity_shift, sizeof(identity_shift));
    assert(scan_occupancy_registration_usable(&occupancy, 42U));

    /* sensor (1.5,0.5,0.5) -> scan (2.5,0.5,0.5): FREE air -> rejected. */
    const localization_point3d_t in_free = { 1.5, 0.5, 0.5 };
    /* sensor (-0.5,0.5,0.5) -> scan (0.5,...): OCCUPIED surface -> allowed. */
    const localization_point3d_t in_occupied = { -0.5, 0.5, 0.5 };
    /* sensor (1.5,2.5,-2.0) -> scan (2.5,2.5,-2.0): UNKNOWN below surface -> allowed. */
    const localization_point3d_t below_surface = { 1.5, 2.5, -2.0 };
    /* sensor (1.5,2.5,-4.0): deeper than the declared candidate volume -> rejected. */
    const localization_point3d_t too_deep = { 1.5, 2.5, -4.0 };
    /* sensor (9,0,0): outside the scanned object -> rejected. */
    const localization_point3d_t outside = { 9.0, 0.0, 0.0 };

    assert(!localization_calculate_scan_candidate_allowed(&in_free, &occupancy));
    assert(localization_calculate_scan_candidate_allowed(&in_occupied, &occupancy));
    assert(localization_calculate_scan_candidate_allowed(&below_surface, &occupancy));
    assert(!localization_calculate_scan_candidate_allowed(&too_deep, &occupancy));
    assert(!localization_calculate_scan_candidate_allowed(&outside, &occupancy));
}


int main(void)
{
    test_scan_candidate_filter_rejects_free_and_outside();
    test_quality_from_residual_matches_the_gaussian_formula();
    test_quality_from_residual_fails_closed_not_nan();
    test_participating_channels_counts_and_masks_computed_pods();
    test_participating_channels_handles_null_geometry();
    test_sensor_position_converts_millimeters_to_meters();
    test_search_bounds_wraps_the_sensor_bounding_box_with_a_margin();
    test_search_bounds_collapses_to_one_layer_when_planar();
    test_search_bounds_handles_zero_positions_without_crashing();
    test_search_bounds_stay_below_surface_and_honor_depth();
    test_redundancy_caps_unverified_confidence();
    test_event_window_covers_signal_and_moveout();
    test_fusion_mapping_groups_picks_and_excludes_energy();
    test_consistency_gate_rejects_physically_impossible_pick();
    test_xcorr_refines_relative_delay_to_sub_sample();
    test_fractional_arrival_time();
    test_geometry_computed_at_ms_takes_the_latest_computed_pod();
    test_arrival_time_us_converts_sample_index_precisely();
    test_traveltime_adapter_common_fields();
    test_traveltime_adapter_insufficient_picks_is_unsolved_with_reason();
    test_traveltime_adapter_null_result_is_unsolved();
    test_semblance_adapter_not_attempted_uses_the_given_reason();
    test_semblance_adapter_found_is_solved();
    test_semblance_adapter_not_found_is_unsolved();
    test_energy_baseline_adapter();
    test_joint_adapter_averages_bounded_quality();
    test_joint_adapter_not_found_is_unsolved();
    test_run_record_to_comparison_method_round_trips_solved();
    test_run_record_to_comparison_method_round_trips_unsolved();
    return 0;
}
