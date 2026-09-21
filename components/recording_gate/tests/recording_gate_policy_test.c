#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "recording_gate.h"


static void test_zero_one_two_assigned_is_insufficient(void)
{
    for (size_t assigned = 0u; assigned <= 2u; ++assigned) {
        const recording_gate_result_t result =
            recording_gate_evaluate(assigned, assigned);
        assert(result.decision == RECORDING_GATE_BLOCKED_INSUFFICIENT_REMOTES);
        assert(result.dimensionality == LOCALIZATION_DIMENSIONALITY_INSUFFICIENT);
        assert(strlen(recording_gate_decision_reason(result.decision)) > 0u);
        assert(strlen(recording_gate_planar_warning(result.decision)) == 0u);
    }
}


static void test_three_assigned_all_level_is_allowed_planar_2d(void)
{
    const recording_gate_result_t result = recording_gate_evaluate(3u, 3u);

    assert(result.decision == RECORDING_GATE_ALLOWED_PLANAR_2D);
    assert(result.dimensionality == LOCALIZATION_DIMENSIONALITY_PLANAR_2D);
    assert(result.assigned_count == 3u);
    assert(result.accepted_count == 3u);
    assert(strlen(recording_gate_decision_reason(result.decision)) == 0u);
    assert(strlen(recording_gate_planar_warning(result.decision)) > 0u);
}


static void test_four_assigned_all_level_is_allowed_3d_capable(void)
{
    const recording_gate_result_t result = recording_gate_evaluate(4u, 4u);

    assert(result.decision == RECORDING_GATE_ALLOWED_3D_CAPABLE);
    assert(result.dimensionality == LOCALIZATION_DIMENSIONALITY_CAPABLE_3D);
    assert(strlen(recording_gate_decision_reason(result.decision)) == 0u);
    assert(strlen(recording_gate_planar_warning(result.decision)) == 0u);
}


static void test_three_assigned_one_not_level_is_blocked(void)
{
    const recording_gate_result_t result = recording_gate_evaluate(3u, 2u);

    assert(result.decision == RECORDING_GATE_BLOCKED_NOT_LEVEL);
    assert(strlen(recording_gate_decision_reason(result.decision)) > 0u);
    assert(strlen(recording_gate_planar_warning(result.decision)) == 0u);
}


static void test_four_assigned_three_level_is_blocked_not_level(void)
{
    const recording_gate_result_t result = recording_gate_evaluate(4u, 3u);

    assert(result.decision == RECORDING_GATE_BLOCKED_NOT_LEVEL);
    /* Dimensionality is still classified from the *assigned* count even
     * while blocked, so a UI could still show "would be 3D-capable once
     * level" without this policy inventing a separate code path. */
    assert(result.dimensionality == LOCALIZATION_DIMENSIONALITY_CAPABLE_3D);
}


static void test_more_than_four_assigned_all_level_is_3d_capable(void)
{
    const recording_gate_result_t result = recording_gate_evaluate(8u, 8u);

    assert(result.decision == RECORDING_GATE_ALLOWED_3D_CAPABLE);
    assert(result.dimensionality == LOCALIZATION_DIMENSIONALITY_CAPABLE_3D);
}


int main(void)
{
    test_zero_one_two_assigned_is_insufficient();
    test_three_assigned_all_level_is_allowed_planar_2d();
    test_four_assigned_all_level_is_allowed_3d_capable();
    test_three_assigned_one_not_level_is_blocked();
    test_four_assigned_three_level_is_blocked_not_level();
    test_more_than_four_assigned_all_level_is_3d_capable();
    puts("recording gate policy tests passed");
    return 0;
}
