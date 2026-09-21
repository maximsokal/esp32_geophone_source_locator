#pragma once

#include <stddef.h>

/*
 * MAX-170: project-wide sensor-count policy, reusable by every localization
 * backend (the existing 3D TDOA solver today, future 2D/Arosio backends
 * later) and by UI. Pure and stateless -- no measured constant lives here,
 * only the participant-count thresholds the ticket itself specifies.
 */

#define LOCALIZATION_DIMENSIONALITY_MIN_PLANAR_2D 3u


typedef enum
{
    /* Fewer than LOCALIZATION_DIMENSIONALITY_MIN_PLANAR_2D participating
     * REMOTEs: no localization method can run, and recording admission
     * itself should already have refused this (MAX-165). */
    LOCALIZATION_DIMENSIONALITY_INSUFFICIENT = 0,

    /* Exactly 3 participating REMOTEs: planar/2D-only methods may run, with
     * an explicit higher-uncertainty label. Never fabricate a Z/depth
     * estimate at this dimensionality. */
    LOCALIZATION_DIMENSIONALITY_PLANAR_2D,

    /* 4 or more participating REMOTEs: 3D-capable methods may run, subject
     * to their own geometry/conditioning/quality gates (e.g. the existing
     * TDOA solver's own coplanarity check). */
    LOCALIZATION_DIMENSIONALITY_CAPABLE_3D

} localization_dimensionality_t;


localization_dimensionality_t localization_dimensionality_classify(
    size_t participating_remote_count
);


/* What a given localization method needs in order to run at all. */
typedef enum
{
    LOCALIZATION_METHOD_REQUIRES_3D = 0,
    LOCALIZATION_METHOD_SUPPORTS_2D_AND_3D

} localization_method_dimensionality_requirement_t;


typedef enum
{
    LOCALIZATION_METHOD_RUNNABLE = 0,

    /* Dimensionality itself is INSUFFICIENT: no method can run, regardless
     * of what it declares support for. */
    LOCALIZATION_METHOD_SKIPPED_INSUFFICIENT_SENSORS,

    /* Dimensionality is PLANAR_2D, but this method declared
     * LOCALIZATION_METHOD_REQUIRES_3D. */
    LOCALIZATION_METHOD_SKIPPED_REQUIRES_3D

} localization_method_availability_t;


localization_method_availability_t localization_method_availability(
    localization_dimensionality_t dimensionality,
    localization_method_dimensionality_requirement_t method_requirement
);


/* A short, stable, human/log-friendly reason string for a skipped method.
 * Returns "" (never NULL) for LOCALIZATION_METHOD_RUNNABLE. */
const char *localization_method_availability_reason(
    localization_method_availability_t availability
);
