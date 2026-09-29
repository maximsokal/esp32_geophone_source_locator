#include <assert.h>
#include <math.h>

#include "localization_velocity_bounds.h"

static localization_velocity_bounds_t valid_bounds(void)
{
    return (localization_velocity_bounds_t) {
        .v_min_mps = 100.0f,
        .v_max_mps = 500.0f,
        .vp_vs_min = 1.5f,
        .vp_vs_max = 2.5f,
        .set = true,
    };
}

static void test_valid_range_and_fixed_speed(void)
{
    localization_velocity_bounds_t bounds = valid_bounds();
    assert(localization_velocity_bounds_validate(&bounds) == LOCALIZATION_VELOCITY_BOUNDS_VALID);
    bounds.v_min_mps = 50.0f;
    bounds.v_max_mps = 500.0f;
    bounds.vp_vs_min = 0.0f;
    bounds.vp_vs_max = 0.0f;
    assert(localization_velocity_bounds_validate(&bounds) == LOCALIZATION_VELOCITY_BOUNDS_VALID);
    bounds.v_min_mps = 800.0f;
    bounds.v_max_mps = 8000.0f;
    assert(localization_velocity_bounds_validate(&bounds) == LOCALIZATION_VELOCITY_BOUNDS_VALID);
    bounds.v_min_mps = 250.0f;
    bounds.v_max_mps = 250.0f;
    assert(localization_velocity_bounds_validate(&bounds) == LOCALIZATION_VELOCITY_BOUNDS_VALID);
}

static void test_validation_reason_codes(void)
{
    localization_velocity_bounds_t bounds = valid_bounds();
    assert(localization_velocity_bounds_validate(NULL) == LOCALIZATION_VELOCITY_BOUNDS_REASON_NULL);
    bounds.set = false;
    assert(localization_velocity_bounds_validate(&bounds) == LOCALIZATION_VELOCITY_BOUNDS_REASON_UNSET);

    bounds = valid_bounds();
    bounds.v_min_mps = NAN;
    assert(localization_velocity_bounds_validate(&bounds) == LOCALIZATION_VELOCITY_BOUNDS_REASON_NON_FINITE);
    bounds = valid_bounds();
    bounds.v_max_mps = INFINITY;
    assert(localization_velocity_bounds_validate(&bounds) == LOCALIZATION_VELOCITY_BOUNDS_REASON_NON_FINITE);
    bounds = valid_bounds();
    bounds.vp_vs_min = NAN;
    assert(localization_velocity_bounds_validate(&bounds) == LOCALIZATION_VELOCITY_BOUNDS_REASON_NON_FINITE);
    bounds = valid_bounds();
    bounds.vp_vs_max = INFINITY;
    assert(localization_velocity_bounds_validate(&bounds) == LOCALIZATION_VELOCITY_BOUNDS_REASON_NON_FINITE);

    bounds = valid_bounds();
    bounds.v_min_mps = 49.0f;
    assert(localization_velocity_bounds_validate(&bounds) == LOCALIZATION_VELOCITY_BOUNDS_REASON_VELOCITY_MIN);
    bounds = valid_bounds();
    bounds.v_max_mps = 8001.0f;
    assert(localization_velocity_bounds_validate(&bounds) == LOCALIZATION_VELOCITY_BOUNDS_REASON_VELOCITY_MAX);
    bounds = valid_bounds();
    bounds.v_min_mps = 501.0f;
    assert(localization_velocity_bounds_validate(&bounds) == LOCALIZATION_VELOCITY_BOUNDS_REASON_VELOCITY_ORDER);
    bounds = valid_bounds();
    bounds.v_min_mps = 50.0f;
    bounds.v_max_mps = 500.1f;
    assert(localization_velocity_bounds_validate(&bounds) == LOCALIZATION_VELOCITY_BOUNDS_REASON_VELOCITY_WIDTH);

    bounds = valid_bounds();
    bounds.vp_vs_min = 1.5f;
    bounds.vp_vs_max = 0.0f;
    assert(localization_velocity_bounds_validate(&bounds) == LOCALIZATION_VELOCITY_BOUNDS_REASON_VP_VS_PAIR);
    bounds = valid_bounds();
    bounds.vp_vs_min = 1.0f;
    assert(localization_velocity_bounds_validate(&bounds) == LOCALIZATION_VELOCITY_BOUNDS_REASON_VP_VS_MIN);
    bounds = valid_bounds();
    bounds.vp_vs_min = 2.0f;
    bounds.vp_vs_max = 1.5f;
    assert(localization_velocity_bounds_validate(&bounds) == LOCALIZATION_VELOCITY_BOUNDS_REASON_VP_VS_ORDER);
    bounds = valid_bounds();
    bounds.vp_vs_max = 10.1f;
    assert(localization_velocity_bounds_validate(&bounds) == LOCALIZATION_VELOCITY_BOUNDS_REASON_VP_VS_MAX);
}

int main(void)
{
    test_valid_range_and_fixed_speed();
    test_validation_reason_codes();
    return 0;
}
