#pragma once

#include <math.h>
#include <stdbool.h>

typedef struct
{
    float v_min_mps;
    float v_max_mps;
    float vp_vs_min;
    float vp_vs_max;
    bool set;
} localization_velocity_bounds_t;

typedef enum
{
    LOCALIZATION_VELOCITY_BOUNDS_VALID = 0,
    LOCALIZATION_VELOCITY_BOUNDS_REASON_NULL,
    LOCALIZATION_VELOCITY_BOUNDS_REASON_UNSET,
    LOCALIZATION_VELOCITY_BOUNDS_REASON_NON_FINITE,
    LOCALIZATION_VELOCITY_BOUNDS_REASON_VELOCITY_MIN,
    LOCALIZATION_VELOCITY_BOUNDS_REASON_VELOCITY_MAX,
    LOCALIZATION_VELOCITY_BOUNDS_REASON_VELOCITY_ORDER,
    LOCALIZATION_VELOCITY_BOUNDS_REASON_VELOCITY_WIDTH,
    LOCALIZATION_VELOCITY_BOUNDS_REASON_VP_VS_PAIR,
    LOCALIZATION_VELOCITY_BOUNDS_REASON_VP_VS_MIN,
    LOCALIZATION_VELOCITY_BOUNDS_REASON_VP_VS_ORDER,
    LOCALIZATION_VELOCITY_BOUNDS_REASON_VP_VS_MAX
} localization_velocity_bounds_reason_t;

/* Equal velocity limits are valid and mean fixed-speed mode. */
static inline localization_velocity_bounds_reason_t localization_velocity_bounds_validate(
    const localization_velocity_bounds_t *bounds
)
{
    if (bounds == NULL) return LOCALIZATION_VELOCITY_BOUNDS_REASON_NULL;
    if (!isfinite(bounds->v_min_mps) || !isfinite(bounds->v_max_mps) ||
        !isfinite(bounds->vp_vs_min) || !isfinite(bounds->vp_vs_max)) {
        return LOCALIZATION_VELOCITY_BOUNDS_REASON_NON_FINITE;
    }
    if (!bounds->set) return LOCALIZATION_VELOCITY_BOUNDS_REASON_UNSET;
    if (bounds->v_min_mps < 50.0f) return LOCALIZATION_VELOCITY_BOUNDS_REASON_VELOCITY_MIN;
    if (bounds->v_max_mps > 8000.0f) return LOCALIZATION_VELOCITY_BOUNDS_REASON_VELOCITY_MAX;
    if (bounds->v_min_mps > bounds->v_max_mps) return LOCALIZATION_VELOCITY_BOUNDS_REASON_VELOCITY_ORDER;
    if (bounds->v_min_mps != bounds->v_max_mps &&
        bounds->v_max_mps / bounds->v_min_mps > 10.0f) {
        return LOCALIZATION_VELOCITY_BOUNDS_REASON_VELOCITY_WIDTH;
    }

    const bool vp_vs_min_set = bounds->vp_vs_min != 0.0f;
    const bool vp_vs_max_set = bounds->vp_vs_max != 0.0f;
    if (vp_vs_min_set != vp_vs_max_set) return LOCALIZATION_VELOCITY_BOUNDS_REASON_VP_VS_PAIR;
    if (!vp_vs_min_set) return LOCALIZATION_VELOCITY_BOUNDS_VALID;
    if (bounds->vp_vs_min <= 1.0f) return LOCALIZATION_VELOCITY_BOUNDS_REASON_VP_VS_MIN;
    if (bounds->vp_vs_min > bounds->vp_vs_max) return LOCALIZATION_VELOCITY_BOUNDS_REASON_VP_VS_ORDER;
    if (bounds->vp_vs_max > 10.0f) return LOCALIZATION_VELOCITY_BOUNDS_REASON_VP_VS_MAX;
    return LOCALIZATION_VELOCITY_BOUNDS_VALID;
}
