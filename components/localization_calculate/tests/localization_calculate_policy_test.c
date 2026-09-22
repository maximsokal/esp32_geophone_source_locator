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
        positions, 3U, LOCALIZATION_DIMENSIONALITY_CAPABLE_3D,
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
        positions, 3U, LOCALIZATION_DIMENSIONALITY_PLANAR_2D,
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
        NULL, 0U, LOCALIZATION_DIMENSIONALITY_CAPABLE_3D,
        &semblance_bounds, &traveltime_bounds
    );
    assert(semblance_bounds.max_corner.x_m > semblance_bounds.min_corner.x_m);
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


int main(void)
{
    test_quality_from_residual_matches_the_gaussian_formula();
    test_quality_from_residual_fails_closed_not_nan();
    test_participating_channels_counts_and_masks_computed_pods();
    test_participating_channels_handles_null_geometry();
    test_sensor_position_converts_millimeters_to_meters();
    test_search_bounds_wraps_the_sensor_bounding_box_with_a_margin();
    test_search_bounds_collapses_to_one_layer_when_planar();
    test_search_bounds_handles_zero_positions_without_crashing();
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
