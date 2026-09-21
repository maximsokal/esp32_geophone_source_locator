#include "localization_dimensionality_policy.h"


localization_dimensionality_t localization_dimensionality_classify(
    size_t participating_remote_count
)
{
    if (participating_remote_count < LOCALIZATION_DIMENSIONALITY_MIN_PLANAR_2D) {
        return LOCALIZATION_DIMENSIONALITY_INSUFFICIENT;
    }

    if (participating_remote_count == LOCALIZATION_DIMENSIONALITY_MIN_PLANAR_2D) {
        return LOCALIZATION_DIMENSIONALITY_PLANAR_2D;
    }

    return LOCALIZATION_DIMENSIONALITY_CAPABLE_3D;
}


localization_method_availability_t localization_method_availability(
    localization_dimensionality_t dimensionality,
    localization_method_dimensionality_requirement_t method_requirement
)
{
    if (dimensionality == LOCALIZATION_DIMENSIONALITY_INSUFFICIENT) {
        return LOCALIZATION_METHOD_SKIPPED_INSUFFICIENT_SENSORS;
    }

    if (
        dimensionality == LOCALIZATION_DIMENSIONALITY_PLANAR_2D &&
        method_requirement == LOCALIZATION_METHOD_REQUIRES_3D
    ) {
        return LOCALIZATION_METHOD_SKIPPED_REQUIRES_3D;
    }

    return LOCALIZATION_METHOD_RUNNABLE;
}


const char *localization_method_availability_reason(
    localization_method_availability_t availability
)
{
    switch (availability) {
        case LOCALIZATION_METHOD_SKIPPED_INSUFFICIENT_SENSORS:
            return
                "fewer than 3 participating REMOTE geophones: "
                "no localization method can run";

        case LOCALIZATION_METHOD_SKIPPED_REQUIRES_3D:
            return
                "exactly 3 participating REMOTE geophones (planar/2D only): "
                "this method requires 4+ for a 3D-capable solve";

        case LOCALIZATION_METHOD_RUNNABLE:
        default:
            return "";
    }
}
