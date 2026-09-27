#pragma once

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "localization_run_record.h"

#define FAILURE_CLASS_BIT(class_value) (1U << (unsigned)(class_value))
#define PICK_CLASS FAILURE_CLASS_BIT(LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN)
#define GEOMETRY_CLASS FAILURE_CLASS_BIT(LOCALIZATION_FAILURE_CLASS_MOVE_SENSORS)
#define INTERNAL_CLASS FAILURE_CLASS_BIT(LOCALIZATION_FAILURE_CLASS_INTERNAL)
#define CALLER_CLASS (PICK_CLASS | GEOMETRY_CLASS | INTERNAL_CLASS)

typedef struct
{
    const char *reason_prefix;
    unsigned allowed_classes;
} localization_calculate_test_reason_class_t;

/* Prefix table mirrors the complete producer reason inventory in
 * docs/localization-failure-routing.md. It only validates emitted classes;
 * production always attaches the class at the reason producer. */
static const localization_calculate_test_reason_class_t localization_calculate_test_reason_classes[] = {
    { "fewer than 3 participating REMOTE geophones", PICK_CLASS | GEOMETRY_CLASS },
    { "exactly 3 participating REMOTE geophones", GEOMETRY_CLASS },
    { "TDOA solver rejected the input", GEOMETRY_CLASS },
    { "TDOA event bridge call failed", INTERNAL_CLASS },
    { "traveltime inversion call failed", INTERNAL_CLASS },
    { "scan occupancy excluded every candidate (verified FREE air", GEOMETRY_CLASS },
    { "insufficient accepted picks for traveltime inversion", PICK_CLASS },
    { "semblance search was not attempted", CALLER_CLASS },
    { "semblance search found no coherent candidate", PICK_CLASS },
    { "semblance search call failed", INTERNAL_CLASS },
    { "no accepted arrival pick to anchor the search window", PICK_CLASS },
    { "energy baseline call failed", INTERNAL_CLASS },
    { "no accepted arrival pick to define the event energy window", PICK_CLASS },
    { "no channel had positive weighted energy", PICK_CLASS },
    { "no individual method solved with usable quality", INTERNAL_CLASS },
    { "TDCAS needs ", PICK_CLASS },
    { "TDCAS was not attempted", PICK_CLASS },
    { "TDCAS solver call failed", INTERNAL_CLASS },
    { "scan occupancy excluded every sensor combination", GEOMETRY_CLASS },
    { "no sensor combination gave a physical velocity-free solution", GEOMETRY_CLASS },
    { "PSAFUVS was not attempted", PICK_CLASS | INTERNAL_CLASS },
    { "PSAFUVS needs 4 channels", PICK_CLASS },
    { "PSAFUVS needs 5 channels", PICK_CLASS },
    { "P/S fit is not physical", PICK_CLASS },
    { "scan voids obstruct too many straight rays for PSAFUVS", GEOMETRY_CLASS },
    { "PSAFUVS solution lies in scan-verified FREE air", GEOMETRY_CLASS },
    { "P and S arrivals do not describe one source", PICK_CLASS },
    { "S-wave picking not configured", PICK_CLASS },
    { "no channel has an accepted S pick after its P pick", PICK_CLASS },
    { "PSAFUVS solver call failed", INTERNAL_CLASS },
    { "SUEA was not attempted", PICK_CLASS },
    { "fewer than 3 usable picks: some sensors sit inside scan-verified empty space", GEOMETRY_CLASS },
    { "fewer than 3 accepted picks for SUEA", PICK_CLASS },
    { "scan occupancy excluded every reachable grid point", GEOMETRY_CLASS },
    { "no grid point is reachable from every sensor with a physical fit", GEOMETRY_CLASS },
    { "SUEA grid model could not be built", INTERNAL_CLASS },
    { "SUEA solver call failed", INTERNAL_CLASS },
    { "only ", PICK_CLASS },
    { "cross-correlation TDOA call failed", INTERNAL_CLASS },
    { "no accepted arrival pick to anchor the correlation windows", PICK_CLASS },
    { "fewer than 2 participating sensors for cross-correlation TDOA", GEOMETRY_CLASS },
    { "cross-correlation TDOA input rejected", INTERNAL_CLASS },
    { "pair delays do not determine the source (underdetermined geometry)", GEOMETRY_CLASS },
    { "robust fit needs ", PICK_CLASS },
    { "robust fit was not attempted", INTERNAL_CLASS },
    { "no accepted arrival pick for robust fit", PICK_CLASS },
    { "scan occupancy excluded every candidate", GEOMETRY_CLASS },
    { "robust fit call failed", INTERNAL_CLASS },
    { "joint P-TDOA + P-S fit was not attempted", CALLER_CLASS },
    { "too few P picks and S-P intervals for the joint fit's unknowns", PICK_CLASS },
    { "no candidate gives physical velocities (vS < vP)", PICK_CLASS },
    { "underdetermined: pods on one circle", GEOMETRY_CLASS },
    { "joint P-TDOA and P-S fit call failed", INTERNAL_CLASS },
    { "eikonal location was not attempted", CALLER_CLASS },
    { "too few picked sensors are reachable in the velocity model", PICK_CLASS | GEOMETRY_CLASS },
    { "no candidate reaches every picked sensor around the voids", GEOMETRY_CLASS },
    { "eikonal model could not be built", INTERNAL_CLASS },
    { "eikonal location call failed", INTERNAL_CLASS },
    { "Bayesian location was not attempted", CALLER_CLASS },
    { "too few accepted P picks for the posterior", PICK_CLASS },
    { "Bayesian location call failed", INTERNAL_CLASS },
    { "scan occupancy artifact unreadable or invalid: constraint not applied", INTERNAL_CLASS },
    { "no valid sensor<->scan registration for this session geometry", GEOMETRY_CLASS },
    { "constraint not applied", GEOMETRY_CLASS | INTERNAL_CLASS },
    { "scan occupancy excluded every candidate or the search failed", GEOMETRY_CLASS },
    { "scan-constrained semblance search failed", INTERNAL_CLASS },
    { "no accepted arrival pick", PICK_CLASS },
    { "eikonal fields around the scan voids could not be computed", INTERNAL_CLASS },
    { "scan occupancy excluded every candidate around the voids", GEOMETRY_CLASS },
    { "eikonal-focused semblance call failed", INTERNAL_CLASS },
    { "not run: the calculation time budget was used up", INTERNAL_CLASS },
    { "no accepted arrival pick to anchor the correlation windows", PICK_CLASS },
};

static void localization_calculate_test_assert_rejection_class(
    const localization_run_record_t *record,
    localization_failure_class_t expected
)
{
    assert(record != NULL && !record->solved);
    assert(record->rejection_reason[0] != '\0');
    assert(record->failure_class == expected);
    bool found = false;
    for (size_t i = 0U; i < sizeof(localization_calculate_test_reason_classes) /
        sizeof(localization_calculate_test_reason_classes[0]); ++i) {
        const char *prefix = localization_calculate_test_reason_classes[i].reason_prefix;
        if (strncmp(record->rejection_reason, prefix, strlen(prefix)) == 0) {
            assert((localization_calculate_test_reason_classes[i].allowed_classes & FAILURE_CLASS_BIT(expected)) != 0U);
            found = true;
            break;
        }
    }
    if (!found) {
        fprintf(stderr, "unmapped rejection reason: %s\n", record->rejection_reason);
    }
    assert(found);
}

#undef FAILURE_CLASS_BIT
#undef PICK_CLASS
#undef GEOMETRY_CLASS
#undef INTERNAL_CLASS
#undef CALLER_CLASS
