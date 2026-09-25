#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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


/* ---- Velocity-free methods (TDCAS, PSAFUVS, SUEA) ------------------------ */

static void test_tdcas_adapter_reports_estimated_velocity_and_reasons(void)
{
    const localization_calculate_run_context_t context = make_context();
    localization_tdcas_result_t result;
    memset(&result, 0, sizeof(result));
    result.status = LOCALIZATION_TDCAS_SOLVED;
    result.position = (localization_point3d_t) { 1.0, 2.0, -1.5 };
    result.velocity_mps = 236.5;
    result.emission_time_us = 777;
    result.rms_residual_us = 10.0;
    localization_run_record_t record;
    localization_calculate_tdcas_to_run_record(&context, &result, 0x3FU, 0xC0U, 2U, 10.0, &record);
    assert(record.solved);
    assert(strcmp(record.method_name, "tdcas") == 0);
    assert(record.kind == LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD);
    assert(record.estimated_p_velocity_mps == 236.5);
    assert(record.estimated_s_velocity_mps == 0.0);
    assert(record.emission_time_us == 777);
    assert(nearly_equal(record.confidence_score, exp(-0.5), 1e-12));
    assert(record.channel_mask_used == 0x3FU && record.channel_mask_rejected == 0xC0U);
    /* The caller's model stays on record: TDCAS simply did not use it. */
    assert(record.propagation_speed_mps == 340.0);

    /* The 4-pod hardware: the reason names the minimum and the count. */
    result.status = LOCALIZATION_TDCAS_INSUFFICIENT_SENSORS;
    result.planar_layout = true;
    result.sensors_used = 4U;
    localization_calculate_tdcas_to_run_record(&context, &result, 0xFU, 0U, 2U, 10.0, &record);
    assert(!record.solved);
    assert(strstr(record.rejection_reason, "needs 5") != NULL);
    assert(strstr(record.rejection_reason, "have 4") != NULL);
    result.status = LOCALIZATION_TDCAS_NO_ALLOWED_COMBINATION;
    localization_calculate_tdcas_to_run_record(&context, &result, 0xFU, 0U, 2U, 10.0, &record);
    assert(strstr(record.rejection_reason, "scan occupancy") != NULL);
    localization_calculate_tdcas_to_run_record(&context, NULL, 0xFU, 0U, 2U, 10.0, &record);
    assert(!record.solved && record.rejection_reason[0] != '\0');
}


static void test_psafuvs_adapter_reports_both_velocities_and_reasons(void)
{
    const localization_calculate_run_context_t context = make_context();
    localization_psafuvs_result_t result;
    memset(&result, 0, sizeof(result));
    result.status = LOCALIZATION_PSAFUVS_SOLVED;
    result.position = (localization_point3d_t) { 0.5, 0.5, -1.0 };
    result.p_velocity_mps = 320.0;
    result.s_velocity_mps = 170.0;
    result.rms_residual_us = 0.0;
    result.iterations = 4U;
    result.converged = true;
    localization_run_record_t record;
    localization_calculate_psafuvs_to_run_record(&context, &result, NULL, 0xFU, 0U, 1U, 20.0, &record);
    assert(record.solved);
    assert(strcmp(record.method_name, "psafuvs") == 0);
    assert(record.estimated_p_velocity_mps == 320.0 && record.estimated_s_velocity_mps == 170.0);
    assert(record.iterations == 4U && record.converged);
    assert(nearly_equal(record.confidence_score, 1.0, 1e-12));

    /* Not attempted: the caller's reason is kept verbatim. */
    localization_calculate_psafuvs_to_run_record(
        &context, NULL, "S-wave picking not configured", 0U, 0xFU, 1U, 20.0, &record
    );
    assert(!record.solved);
    assert(strcmp(record.rejection_reason, "S-wave picking not configured") == 0);

    const localization_psafuvs_status_t statuses[] = {
        LOCALIZATION_PSAFUVS_INSUFFICIENT_SENSORS, LOCALIZATION_PSAFUVS_DEGENERATE,
        LOCALIZATION_PSAFUVS_NONPHYSICAL_VELOCITIES, LOCALIZATION_PSAFUVS_RAYS_OBSTRUCTED,
        LOCALIZATION_PSAFUVS_NO_ALLOWED_CANDIDATE,
    };
    for (size_t i = 0U; i < sizeof(statuses) / sizeof(statuses[0]); ++i) {
        result.status = statuses[i];
        localization_calculate_psafuvs_to_run_record(&context, &result, NULL, 0xFU, 0U, 1U, 20.0, &record);
        assert(!record.solved && record.rejection_reason[0] != '\0');
        assert(record.estimated_p_velocity_mps == 0.0);
    }
}


static void test_suea_adapter_and_redundancy(void)
{
    const localization_calculate_run_context_t context = make_context();
    localization_suea_result_t result;
    memset(&result, 0, sizeof(result));
    result.status = LOCALIZATION_SUEA_SOLVED;
    result.best_position = (localization_point3d_t) { 2.0, 1.0, -0.8 };
    result.best_residual_rms_us = 20.0;
    result.velocity_mps = 251.0;
    localization_run_record_t record;
    localization_calculate_suea_to_run_record(&context, &result, 0xFU, 0U, 1U, 20.0, &record);
    assert(record.solved && strcmp(record.method_name, "suea") == 0);
    assert(record.estimated_p_velocity_mps == 251.0);
    assert(nearly_equal(record.confidence_score, exp(-0.5), 1e-12));

    result.status = LOCALIZATION_SUEA_INSUFFICIENT_PICKS;
    result.pick_mask_unreachable = 0x2U;
    localization_calculate_suea_to_run_record(&context, &result, 0xDU, 0x2U, 1U, 20.0, &record);
    assert(!record.solved && strstr(record.rejection_reason, "empty space") != NULL);
    result.status = LOCALIZATION_SUEA_NO_ALLOWED_CANDIDATE;
    localization_calculate_suea_to_run_record(&context, &result, 0xDU, 0x2U, 1U, 20.0, &record);
    assert(strstr(record.rejection_reason, "scan occupancy") != NULL);

    /* Velocity-free redundancy: 4 P picks cannot verify 5 unknowns;
     * 4 P + 4 S picks leave 2 degrees of freedom for PSAFUVS. */
    assert(localization_calculate_velocity_free_degrees_of_freedom(4U, 5U) == -1);
    assert(localization_calculate_velocity_free_degrees_of_freedom(8U, 6U) == 2);
    assert(localization_calculate_velocity_free_degrees_of_freedom(0U, 5U) == -1);
}


static void test_velocity_free_methods_share_the_pick_evidence_group(void)
{
    const char *names[] = { "tdcas", "psafuvs", "suea" };
    for (size_t i = 0U; i < 3U; ++i) {
        localization_run_record_t record;
        memset(&record, 0, sizeof(record));
        snprintf(record.method_name, sizeof(record.method_name), "%s", names[i]);
        record.solved = true;
        record.confidence_score = 0.8;
        localization_comparison_method_result_t method;
        localization_calculate_run_record_to_comparison_method(&record, &method);
        assert(method.evidence_group == LOCALIZATION_CALCULATE_PICK_EVIDENCE_GROUP);
        assert(method.quality == 0.8);
    }
}


static void test_suea_grid_bounds_extend_the_search_box(void)
{
    localization_traveltime_search_bounds_t search = {
        .min_corner = { -1.0, -2.0, -3.0 },
        .max_corner = { 4.0, 5.0, 0.2 },
        .step_m = 0.1,
    };
    localization_point3d_t lo, hi;
    localization_calculate_suea_grid_bounds(&search, &lo, &hi);
    assert(nearly_equal(lo.x_m, -1.1, 1e-12) && nearly_equal(lo.y_m, -2.1, 1e-12) && lo.z_m == -3.0);
    assert(nearly_equal(hi.x_m, 4.1, 1e-12) && nearly_equal(hi.y_m, 5.1, 1e-12) && nearly_equal(hi.z_m, 0.3, 1e-12));
    /* Planar stays one layer. */
    search.min_corner.z_m = search.max_corner.z_m = 0.05;
    localization_calculate_suea_grid_bounds(&search, &lo, &hi);
    assert(lo.z_m == 0.05 && hi.z_m == 0.05);
}


static void test_scan_sectors_map_to_suea_cells_and_ray_screening(void)
{
    /* Same synthetic scan as the candidate filter test: voxel 1 m, FREE
     * (2,0,0), OCCUPIED (0,0,0), envelope z <= 1, registration +1 m in x. */
    int64_t free_keys[2] = { test_pack_key(2, 0, 0), test_pack_key(3, 0, 0) };
    int64_t occupied_keys[1] = { test_pack_key(0, 0, 0) };
    scan_occupancy_t occupancy;
    memset(&occupancy, 0, sizeof(occupancy));
    occupancy.voxel_size_m = 1.0;
    occupancy.below_surface_m = 2.0;
    occupancy.has_envelope = true;
    occupancy.envelope_min[0] = -1.0; occupancy.envelope_min[1] = -1.0; occupancy.envelope_min[2] = -1.0;
    occupancy.envelope_max[0] = 6.0; occupancy.envelope_max[1] = 4.0; occupancy.envelope_max[2] = 1.0;
    occupancy.free_keys = free_keys;
    occupancy.free_count = 2U;
    occupancy.occupied_keys = occupied_keys;
    occupancy.occupied_count = 1U;
    occupancy.registration.present = true;
    occupancy.registration.geometry_computed_at_ms = 42U;
    const double shift[16] = { 1, 0, 0, 1,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
    memcpy(occupancy.registration.sensor_to_scan, shift, sizeof(shift));

    /* FREE air (empty sector) blocks waves; OCCUPIED (filled) is solid;
     * the unseen interior stays passable UNKNOWN, never empty. */
    const localization_point3d_t free_air = { 1.5, 0.5, 0.5 };
    const localization_point3d_t surface = { -0.5, 0.5, 0.5 };
    const localization_point3d_t interior = { 1.5, 2.5, -1.5 };
    const localization_point3d_t above_scan = { 1.5, 2.5, 3.0 };
    const localization_point3d_t beside_scan = { 9.0, 0.5, 0.5 };
    assert(localization_calculate_scan_suea_cell(&free_air, &occupancy) == LOCALIZATION_SUEA_CELL_EMPTY);
    assert(localization_calculate_scan_suea_cell(&surface, &occupancy) == LOCALIZATION_SUEA_CELL_SOLID);
    assert(localization_calculate_scan_suea_cell(&interior, &occupancy) == LOCALIZATION_SUEA_CELL_UNKNOWN);
    assert(localization_calculate_scan_suea_cell(&above_scan, &occupancy) == LOCALIZATION_SUEA_CELL_EMPTY);
    assert(localization_calculate_scan_suea_cell(&beside_scan, &occupancy) == LOCALIZATION_SUEA_CELL_UNKNOWN);
    assert(localization_calculate_scan_suea_cell(&free_air, NULL) == LOCALIZATION_SUEA_CELL_UNKNOWN);

    const localization_calculate_scan_ray_context_t ray = { .occupancy = &occupancy, .sensor_clearance_m = 0.5 };
    /* Along y = 0.5, z = 0.5 the ray crosses the two FREE voxels (scan x
     * in [2, 4)): obstructed. */
    const localization_point3d_t sensor = { -0.5, 0.5, 0.5 };
    const localization_point3d_t across_void = { 4.5, 0.5, 0.5 };
    assert(!localization_calculate_scan_ray_clear(&sensor, &across_void, (void *)&ray));
    /* Below the void: clear. */
    const localization_point3d_t below_void = { 4.5, 0.5, -1.5 };
    const localization_point3d_t sensor_low = { -0.5, 0.5, -1.5 };
    assert(localization_calculate_scan_ray_clear(&sensor_low, &below_void, (void *)&ray));
    /* A sensor sitting in FREE air at its own end is ignored (clearance):
     * only the rest of the ray counts. */
    const localization_point3d_t hovering = { 2.4, 0.5, 0.5 };
    const localization_point3d_t down = { 2.4, 0.5, -1.8 };
    assert(localization_calculate_scan_ray_clear(&hovering, &down, (void *)&ray));
    /* No context: every ray is clear (no constraint without a scan). */
    assert(localization_calculate_scan_ray_clear(&sensor, &across_void, NULL));
}


static void test_extended_methods_share_the_pick_evidence_group(void)
{
    const char *grouped[] = { "xcorr_tdoa", "robust_tdoa", "psp_joint", "eikonal_fmm", "bayesian" };
    for (size_t i = 0U; i < 5U; ++i) {
        localization_run_record_t record;
        memset(&record, 0, sizeof(record));
        snprintf(record.method_name, sizeof(record.method_name), "%s", grouped[i]);
        record.solved = true;
        record.confidence_score = 0.7;
        localization_comparison_method_result_t method;
        localization_calculate_run_record_to_comparison_method(&record, &method);
        assert(method.evidence_group == LOCALIZATION_CALCULATE_PICK_EVIDENCE_GROUP);
        assert(method.quality == 0.7);
    }
    /* Scan variants are never fused, so they need no group. */
    const char *scan_only[] = { "eikonal_scan_voids", "bayesian_scan_voids", "semblance_eikonal_voids" };
    for (size_t i = 0U; i < 3U; ++i) {
        localization_run_record_t record;
        memset(&record, 0, sizeof(record));
        snprintf(record.method_name, sizeof(record.method_name), "%s", scan_only[i]);
        record.solved = true;
        localization_comparison_method_result_t method;
        localization_calculate_run_record_to_comparison_method(&record, &method);
        assert(method.evidence_group == 0U);
    }
}


static void test_xcorr_and_robust_adapters(void)
{
    const localization_calculate_run_context_t context = make_context();
    localization_run_record_t record;

    localization_xcorr_tdoa_result_t xcorr;
    memset(&xcorr, 0, sizeof(xcorr));
    xcorr.status = LOCALIZATION_XCORR_TDOA_SOLVED;
    xcorr.position = (localization_point3d_t) { 1.0, 2.0, -0.5 };
    xcorr.rms_pair_residual_us = 20.0;
    localization_calculate_xcorr_tdoa_to_run_record(&context, &xcorr, 0xFU, 0U, 1U, 20.0, &record);
    assert(record.solved && strcmp(record.method_name, "xcorr_tdoa") == 0);
    assert(record.source_position.y_m == 2.0 && record.rms_residual_us == 20.0);
    assert(nearly_equal(record.confidence_score, exp(-0.5), 1e-12));

    xcorr.status = LOCALIZATION_XCORR_TDOA_INSUFFICIENT_PAIRS;
    xcorr.pairs_measured = 1U;
    xcorr.pairs_total = 6U;
    xcorr.channels_used = 2U;
    localization_calculate_xcorr_tdoa_to_run_record(&context, &xcorr, 0x3U, 0xCU, 1U, 20.0, &record);
    assert(!record.solved && strstr(record.rejection_reason, "only 1 of 6 channel pairs") != NULL);
    xcorr.status = LOCALIZATION_XCORR_TDOA_NO_ANCHOR;
    localization_calculate_xcorr_tdoa_to_run_record(&context, &xcorr, 0U, 0xFU, 1U, 20.0, &record);
    assert(!record.solved && strstr(record.rejection_reason, "anchor") != NULL);

    localization_robust_result_t robust;
    memset(&robust, 0, sizeof(robust));
    robust.status = LOCALIZATION_ROBUST_SOLVED;
    robust.position = (localization_point3d_t) { 0.5, 0.5, -1.0 };
    robust.rms_residual_us = 0.0;
    robust.emission_time_us = 42;
    localization_calculate_robust_to_run_record(&context, &robust, 0x7U, 0x8U, 1U, 20.0, &record);
    assert(record.solved && strcmp(record.method_name, "robust_tdoa") == 0);
    assert(record.confidence_score == 1.0 && record.emission_time_us == 42);
    assert(record.channel_mask_used == 0x7U && record.channel_mask_rejected == 0x8U);
    /* An inconsistency no pick can be blamed for is not a confirmation. */
    robust.consensus_ambiguous = true;
    localization_calculate_robust_to_run_record(&context, &robust, 0xFU, 0U, 1U, 20.0, &record);
    assert(record.confidence_score == LOCALIZATION_CALCULATE_UNVERIFIED_CONFIDENCE_CAP);
    robust.status = LOCALIZATION_ROBUST_INSUFFICIENT_OBSERVATIONS;
    robust.planar = false;
    robust.degrees_of_freedom = -1;
    localization_calculate_robust_to_run_record(&context, &robust, 0x7U, 0x8U, 1U, 20.0, &record);
    assert(!record.solved && strstr(record.rejection_reason, "needs 4 accepted P picks for a 3D search, have 3") != NULL);
    localization_calculate_robust_to_run_record(&context, NULL, 0U, 0xFU, 1U, 20.0, &record);
    assert(!record.solved && record.rejection_reason[0] != '\0');
}


static void test_psp_eikonal_and_bayes_adapters(void)
{
    const localization_calculate_run_context_t context = make_context();
    localization_run_record_t record;

    localization_psp_result_t psp;
    memset(&psp, 0, sizeof(psp));
    psp.status = LOCALIZATION_PSP_SOLVED;
    psp.position = (localization_point3d_t) { 1.0, 1.0, -1.0 };
    psp.p_velocity_mps = 320.0;
    psp.s_velocity_mps = 180.0;
    psp.rms_p_residual_us = 20.0;
    localization_calculate_psp_to_run_record(&context, &psp, NULL, 0xFU, 0U, 1U, 20.0, &record);
    assert(record.solved && strcmp(record.method_name, "psp_joint") == 0);
    assert(record.estimated_p_velocity_mps == 320.0 && record.estimated_s_velocity_mps == 180.0);
    psp.status = LOCALIZATION_PSP_UNDERDETERMINED;
    localization_calculate_psp_to_run_record(&context, &psp, NULL, 0xFU, 0U, 1U, 20.0, &record);
    assert(!record.solved && strstr(record.rejection_reason, "underdetermined") != NULL);
    localization_calculate_psp_to_run_record(&context, NULL, "S-wave picking not configured", 0U, 0xFU, 1U, 20.0, &record);
    assert(!record.solved && strcmp(record.rejection_reason, "S-wave picking not configured") == 0);

    localization_eikonal_result_t eikonal;
    memset(&eikonal, 0, sizeof(eikonal));
    eikonal.status = LOCALIZATION_EIKONAL_SOLVED;
    eikonal.position = (localization_point3d_t) { 2.0, 1.0, -0.6 };
    eikonal.velocity_scale = 0.85;
    eikonal.rms_residual_us = 10.0;
    localization_calculate_eikonal_to_run_record(&context, &eikonal, "eikonal_scan_voids", NULL, true,
                                                 0xFU, 0U, 1U, 20.0, &record);
    assert(record.solved && strcmp(record.method_name, "eikonal_scan_voids") == 0);
    assert(nearly_equal(record.estimated_p_velocity_mps, 340.0 / 0.85, 1e-9));
    localization_calculate_eikonal_to_run_record(&context, &eikonal, "eikonal_fmm", NULL, false,
                                                 0xFU, 0U, 1U, 20.0, &record);
    assert(record.estimated_p_velocity_mps == 0.0); /* the scale was fixed */
    eikonal.status = LOCALIZATION_EIKONAL_NO_REACHABLE_CANDIDATE;
    localization_calculate_eikonal_to_run_record(&context, &eikonal, "eikonal_scan_voids", NULL, true,
                                                 0xFU, 0U, 1U, 20.0, &record);
    assert(!record.solved && strstr(record.rejection_reason, "around the voids") != NULL);
    localization_calculate_eikonal_to_run_record(&context, NULL, "eikonal_scan_voids", "constraint not applied", true,
                                                 0xFU, 0U, 1U, 20.0, &record);
    assert(!record.solved && strcmp(record.rejection_reason, "constraint not applied") == 0);

    localization_bayes_result_t bayes;
    memset(&bayes, 0, sizeof(bayes));
    bayes.status = LOCALIZATION_BAYES_SOLVED;
    bayes.map_position = (localization_point3d_t) { 1.1, 1.6, -1.3 };
    bayes.std_m = (localization_point3d_t) { 0.1, 0.1, 0.8 };
    bayes.credible_level = 0.9;
    bayes.credible_volume_m3 = 0.13;
    bayes.credible_min = (localization_point3d_t) { 0.9, 1.4, -3.0 };
    bayes.credible_max = (localization_point3d_t) { 1.3, 1.8, -0.1 };
    bayes.velocity_mean_mps = 333.0;
    bayes.rms_residual_us = 20.0;
    localization_calculate_bayes_to_run_record(&context, &bayes, "bayesian", NULL, true, 0xFU, 0U, 1U, 20.0, &record);
    assert(record.solved && strcmp(record.method_name, "bayesian") == 0);
    assert(record.source_position.z_m == -1.3);
    assert(record.uncertainty.credible_level == 0.9 && record.uncertainty.std_m.z_m == 0.8);
    assert(record.uncertainty.credible_volume_m3 == 0.13 && record.uncertainty.credible_min.z_m == -3.0);
    assert(record.estimated_p_velocity_mps == 333.0);
    /* The record serializes (the uncertainty passes validation). */
    char *json = NULL;
    assert(localization_run_record_to_json(&record, &json) == ESP_OK);
    assert(strstr(json, "\"uncertainty\"") != NULL);
    free(json);
    bayes.status = LOCALIZATION_BAYES_INSUFFICIENT_PICKS;
    localization_calculate_bayes_to_run_record(&context, &bayes, "bayesian", NULL, true, 0x7U, 0x8U, 1U, 20.0, &record);
    assert(!record.solved && record.uncertainty.credible_level == 0.0);
}


static void test_s_minus_p_sigma_grows_as_the_s_onset_weakens(void)
{
    /* Equally clean onsets: sqrt(2) x the pick sigma. */
    assert(nearly_equal(localization_calculate_s_minus_p_sigma_us(20.0, 8.0, 8.0), 20.0 * sqrt(2.0), 1e-9));
    /* A cleaner S than P never makes the interval better than the P pick. */
    assert(nearly_equal(localization_calculate_s_minus_p_sigma_us(20.0, 4.0, 16.0), 20.0 * sqrt(2.0), 1e-9));
    /* S four times weaker: sS = 4 sP. */
    assert(nearly_equal(localization_calculate_s_minus_p_sigma_us(20.0, 16.0, 4.0), 20.0 * sqrt(17.0), 1e-9));
    /* Capped at 10x, and an unknown S SNR takes the cap. */
    assert(nearly_equal(localization_calculate_s_minus_p_sigma_us(20.0, 100.0, 1.0), 20.0 * sqrt(101.0), 1e-9));
    assert(nearly_equal(localization_calculate_s_minus_p_sigma_us(20.0, 8.0, 0.0), 20.0 * sqrt(101.0), 1e-9));
}


static void test_discretization_error(void)
{
    assert(nearly_equal(localization_calculate_discretization_error_us(0.1, 300.0), 0.1 / (300.0 * sqrt(6.0)) * 1e6, 1e-9));
    assert(localization_calculate_discretization_error_us(0.0, 300.0) == 0.0);
    assert(localization_calculate_discretization_error_us(0.1, 0.0) == 0.0);
}


static void test_bayes_adapter_carries_the_inflation(void)
{
    const localization_calculate_run_context_t context = make_context();
    localization_bayes_result_t bayes;
    memset(&bayes, 0, sizeof(bayes));
    bayes.status = LOCALIZATION_BAYES_SOLVED;
    bayes.credible_level = 0.9;
    bayes.sigma_inflation = 4.1;
    localization_run_record_t record;
    localization_calculate_bayes_to_run_record(&context, &bayes, "bayesian", NULL, false, 0xFU, 0U, 1U, 20.0, &record);
    assert(record.uncertainty.sigma_inflation == 4.1);
    bayes.sigma_inflation = 0.0; /* not computed: never below 1 */
    localization_calculate_bayes_to_run_record(&context, &bayes, "bayesian", NULL, false, 0xFU, 0U, 1U, 20.0, &record);
    assert(record.uncertainty.sigma_inflation == 1.0);
}


static void test_extended_bounds_copy_the_search_box(void)
{
    const localization_traveltime_search_bounds_t search = {
        .min_corner = { -1.0, -2.0, -3.0 }, .max_corner = { 4.0, 5.0, 0.2 }, .step_m = 0.1,
    };
    const localization_extended_bounds_t bounds = localization_calculate_extended_bounds(&search);
    assert(bounds.min_corner.y_m == -2.0 && bounds.max_corner.z_m == 0.2 && bounds.step_m == 0.1);
    assert(localization_extended_bounds_valid(&bounds));
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
    test_tdcas_adapter_reports_estimated_velocity_and_reasons();
    test_psafuvs_adapter_reports_both_velocities_and_reasons();
    test_suea_adapter_and_redundancy();
    test_velocity_free_methods_share_the_pick_evidence_group();
    test_suea_grid_bounds_extend_the_search_box();
    test_scan_sectors_map_to_suea_cells_and_ray_screening();
    test_extended_methods_share_the_pick_evidence_group();
    test_xcorr_and_robust_adapters();
    test_psp_eikonal_and_bayes_adapters();
    test_s_minus_p_sigma_grows_as_the_s_onset_weakens();
    test_extended_bounds_copy_the_search_box();
    test_discretization_error();
    test_bayes_adapter_carries_the_inflation();
    return 0;
}
