#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "localization_comparison.h"


static double distance_m(
    localization_point3d_t a,
    localization_point3d_t b
)
{
    const double dx = a.x_m - b.x_m;
    const double dy = a.y_m - b.y_m;
    const double dz = a.z_m - b.z_m;
    return sqrt(dx * dx + dy * dy + dz * dz);
}


static localization_comparison_method_result_t solved_method(
    const char *name,
    localization_point3d_t position,
    double quality
)
{
    localization_comparison_method_result_t method;
    memset(&method, 0, sizeof(method));
    snprintf(method.method_name, sizeof(method.method_name), "%s", name);
    method.solved = true;
    method.position = position;
    method.quality = quality;
    return method;
}


static localization_comparison_method_result_t unsolved_method(
    const char *name,
    const char *reason
)
{
    localization_comparison_method_result_t method;
    memset(&method, 0, sizeof(method));
    snprintf(method.method_name, sizeof(method.method_name), "%s", name);
    method.solved = false;
    snprintf(method.rejection_reason, sizeof(method.rejection_reason), "%s", reason);
    return method;
}


static void test_equal_quality_methods_fuse_to_the_midpoint(void)
{
    const localization_comparison_method_result_t methods[2] = {
        solved_method("tdoa_gauss_newton", (localization_point3d_t) { 0.0, 0.0, 0.0 }, 0.5),
        solved_method("traveltime_inversion", (localization_point3d_t) { 4.0, 0.0, 0.0 }, 0.5)
    };

    localization_comparison_result_t result;
    assert(localization_comparison_fuse(methods, 2u, &result) == ESP_OK);

    assert(result.method_count == 2u);
    assert(result.joint_found);
    assert(fabs(result.joint_position.x_m - 2.0) < 1e-9);
    assert(result.joint_methods_used_count == 2u);
    assert(fabs(result.joint_total_quality - 1.0) < 1e-9);
    assert(fabs(result.max_pairwise_disagreement_m - 4.0) < 1e-9);
}


static void test_a_low_quality_method_cannot_dominate_the_joint_result(void)
{
    const localization_comparison_method_result_t methods[3] = {
        solved_method("tdoa_gauss_newton", (localization_point3d_t) { 1.0, 1.0, 0.0 }, 0.95),
        solved_method("traveltime_inversion", (localization_point3d_t) { 1.05, 0.98, 0.0 }, 0.90),
        /* A wild outlier, but given almost no weight. */
        solved_method("energy_baseline", (localization_point3d_t) { 50.0, 50.0, 0.0 }, 0.01)
    };

    localization_comparison_result_t result;
    assert(localization_comparison_fuse(methods, 3u, &result) == ESP_OK);

    assert(result.joint_found);
    assert(result.joint_methods_used_count == 3u);
    /* The joint estimate must stay close to the two agreeing, high-quality
     * methods, nowhere near the low-quality outlier. */
    assert(result.joint_position.x_m < 2.0);
    assert(result.joint_position.y_m < 2.0);

    const double expected_disagreement = distance_m(
        (localization_point3d_t) { 1.0, 1.0, 0.0 }, (localization_point3d_t) { 50.0, 50.0, 0.0 }
    );
    assert(fabs(result.max_pairwise_disagreement_m - expected_disagreement) < 1e-6);
}


static void test_zero_quality_solved_method_is_excluded_from_joint_but_kept_visible(void)
{
    const localization_comparison_method_result_t methods[2] = {
        solved_method("tdoa_gauss_newton", (localization_point3d_t) { 1.0, 1.0, 0.0 }, 0.8),
        solved_method("energy_baseline", (localization_point3d_t) { 9.0, 9.0, 0.0 }, 0.0)
    };

    localization_comparison_result_t result;
    assert(localization_comparison_fuse(methods, 2u, &result) == ESP_OK);

    assert(result.method_count == 2u);
    assert(result.methods[1].solved);
    assert(fabs(result.methods[1].position.x_m - 9.0) < 1e-9);

    assert(result.joint_found);
    assert(result.joint_methods_used_count == 1u);
    assert(fabs(result.joint_position.x_m - 1.0) < 1e-9);
    assert(fabs(result.joint_position.y_m - 1.0) < 1e-9);

    /* Still visible as disagreement even though it contributed no weight. */
    assert(result.max_pairwise_disagreement_m > 10.0);
}


static void test_mixed_solved_and_unsolved_methods_retain_reasons(void)
{
    const localization_comparison_method_result_t methods[3] = {
        solved_method("tdoa_gauss_newton", (localization_point3d_t) { 1.0, 1.0, 0.0 }, 0.7),
        unsolved_method("traveltime_inversion", "insufficient picks: 2 usable, need 3"),
        unsolved_method("semblance_coherency", "insufficient channels for a waveform search")
    };

    localization_comparison_result_t result;
    assert(localization_comparison_fuse(methods, 3u, &result) == ESP_OK);

    assert(result.method_count == 3u);
    assert(!result.methods[1].solved);
    assert(strcmp(result.methods[1].rejection_reason, "insufficient picks: 2 usable, need 3") == 0);
    assert(!result.methods[2].solved);

    assert(result.joint_found);
    assert(result.joint_methods_used_count == 1u);
    assert(fabs(result.max_pairwise_disagreement_m - 0.0) < 1e-9);
}


static void test_all_unsolved_has_no_joint_result(void)
{
    const localization_comparison_method_result_t methods[2] = {
        unsolved_method("tdoa_gauss_newton", "insufficient channels"),
        unsolved_method("traveltime_inversion", "insufficient channels")
    };

    localization_comparison_result_t result;
    assert(localization_comparison_fuse(methods, 2u, &result) == ESP_OK);

    assert(!result.joint_found);
    assert(result.joint_methods_used_count == 0u);
    assert(result.joint_total_quality == 0.0);
    assert(result.max_pairwise_disagreement_m == 0.0);
}


static void test_documented_quality_recipe_produces_sensible_weighting(void)
{
    /* Plugs in exactly the mappings the header documents, to prove the
     * documented recipe -- not just an arbitrary weight -- gives a sane
     * result: a badly-fit traveltime inversion (huge residual relative to
     * its own timing uncertainty) must end up with negligible influence
     * next to a well-fit TDOA solve and a strong semblance score. */
    const double timing_uncertainty_us = 50.0;

    const double tdoa_residual_us = 40.0;
    const double tdoa_quality = exp(-0.5 * pow(tdoa_residual_us / timing_uncertainty_us, 2.0));

    const double traveltime_residual_us = 2000.0; /* a poor fit */
    const double traveltime_quality =
        exp(-0.5 * pow(traveltime_residual_us / timing_uncertainty_us, 2.0));

    const double semblance_best_semblance = 0.92;
    const double energy_concentration = 0.55;

    const localization_comparison_method_result_t methods[4] = {
        solved_method(
            "tdoa_gauss_newton", (localization_point3d_t) { 2.0, 3.0, 0.0 }, tdoa_quality
        ),
        solved_method(
            "traveltime_inversion", (localization_point3d_t) { 8.0, 8.0, 0.0 }, traveltime_quality
        ),
        solved_method(
            "semblance_coherency", (localization_point3d_t) { 2.1, 2.9, 0.0 },
            semblance_best_semblance
        ),
        solved_method(
            "energy_baseline", (localization_point3d_t) { 3.0, 3.5, 0.0 }, energy_concentration
        )
    };

    localization_comparison_result_t result;
    assert(localization_comparison_fuse(methods, 4u, &result) == ESP_OK);

    /* At this residual/uncertainty ratio the Gaussian quality score
     * underflows to exactly 0.0 -- a real double, not a bug (chi-square
     * 1600 halved is far past exp()'s underflow point). That means the
     * poorly-fit method is fully excluded from the joint fusion, not just
     * heavily down-weighted: an even stronger form of "cannot dominate". */
    assert(traveltime_quality == 0.0);
    assert(result.joint_found);
    assert(result.joint_methods_used_count == 3u);

    printf(
        "[MAX-138] documented-recipe fusion: joint=(%.3f,%.3f) disagreement=%.3f m "
        "tdoa_q=%.4f traveltime_q=%.4g\n",
        result.joint_position.x_m, result.joint_position.y_m,
        result.max_pairwise_disagreement_m, tdoa_quality, traveltime_quality
    );

    /* Dominated by TDOA + semblance (which agree near (2,3)), not pulled
     * toward the poorly-fit traveltime-inversion outlier at (8,8). */
    assert(result.joint_position.x_m < 4.0);
    assert(result.joint_position.y_m < 4.0);
}


static void test_rejects_invalid_arguments(void)
{
    const localization_comparison_method_result_t valid[1] = {
        solved_method("tdoa_gauss_newton", (localization_point3d_t) { 0.0, 0.0, 0.0 }, 0.5)
    };
    localization_comparison_result_t result;

    assert(localization_comparison_fuse(NULL, 1u, &result) == ESP_ERR_INVALID_ARG);
    assert(localization_comparison_fuse(valid, 1u, NULL) == ESP_ERR_INVALID_ARG);
    assert(localization_comparison_fuse(valid, 0u, &result) == ESP_ERR_INVALID_ARG);
    assert(
        localization_comparison_fuse(
            valid, LOCALIZATION_COMPARISON_MAX_METHODS + 1u, &result
        ) == ESP_ERR_INVALID_ARG
    );

    localization_comparison_method_result_t unnamed = valid[0];
    unnamed.method_name[0] = '\0';
    assert(localization_comparison_fuse(&unnamed, 1u, &result) == ESP_ERR_INVALID_ARG);

    localization_comparison_method_result_t unsolved_without_reason =
        unsolved_method("tdoa_gauss_newton", "placeholder");
    unsolved_without_reason.rejection_reason[0] = '\0';
    assert(
        localization_comparison_fuse(&unsolved_without_reason, 1u, &result) ==
        ESP_ERR_INVALID_ARG
    );

    localization_comparison_method_result_t non_finite_position = valid[0];
    non_finite_position.position.x_m = NAN;
    assert(
        localization_comparison_fuse(&non_finite_position, 1u, &result) ==
        ESP_ERR_INVALID_ARG
    );
}


int main(void)
{
    test_equal_quality_methods_fuse_to_the_midpoint();
    test_a_low_quality_method_cannot_dominate_the_joint_result();
    test_zero_quality_solved_method_is_excluded_from_joint_but_kept_visible();
    test_mixed_solved_and_unsolved_methods_retain_reasons();
    test_all_unsolved_has_no_joint_result();
    test_documented_quality_recipe_produces_sensible_weighting();
    test_rejects_invalid_arguments();
    puts("localization comparison tests passed");
    return 0;
}
