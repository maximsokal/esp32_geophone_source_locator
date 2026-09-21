#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "localization_dimensionality_policy.h"


static void test_classify_boundaries(void)
{
    assert(localization_dimensionality_classify(0u) == LOCALIZATION_DIMENSIONALITY_INSUFFICIENT);
    assert(localization_dimensionality_classify(1u) == LOCALIZATION_DIMENSIONALITY_INSUFFICIENT);
    assert(localization_dimensionality_classify(2u) == LOCALIZATION_DIMENSIONALITY_INSUFFICIENT);
    assert(localization_dimensionality_classify(3u) == LOCALIZATION_DIMENSIONALITY_PLANAR_2D);
    assert(localization_dimensionality_classify(4u) == LOCALIZATION_DIMENSIONALITY_CAPABLE_3D);
    assert(localization_dimensionality_classify(8u) == LOCALIZATION_DIMENSIONALITY_CAPABLE_3D);
    assert(localization_dimensionality_classify(100u) == LOCALIZATION_DIMENSIONALITY_CAPABLE_3D);
}


static void test_insufficient_blocks_every_method_regardless_of_requirement(void)
{
    assert(
        localization_method_availability(
            LOCALIZATION_DIMENSIONALITY_INSUFFICIENT,
            LOCALIZATION_METHOD_REQUIRES_3D
        ) == LOCALIZATION_METHOD_SKIPPED_INSUFFICIENT_SENSORS
    );
    assert(
        localization_method_availability(
            LOCALIZATION_DIMENSIONALITY_INSUFFICIENT,
            LOCALIZATION_METHOD_SUPPORTS_2D_AND_3D
        ) == LOCALIZATION_METHOD_SKIPPED_INSUFFICIENT_SENSORS
    );
}


static void test_planar_2d_skips_3d_only_methods_but_runs_flexible_ones(void)
{
    assert(
        localization_method_availability(
            LOCALIZATION_DIMENSIONALITY_PLANAR_2D,
            LOCALIZATION_METHOD_REQUIRES_3D
        ) == LOCALIZATION_METHOD_SKIPPED_REQUIRES_3D
    );
    assert(
        localization_method_availability(
            LOCALIZATION_DIMENSIONALITY_PLANAR_2D,
            LOCALIZATION_METHOD_SUPPORTS_2D_AND_3D
        ) == LOCALIZATION_METHOD_RUNNABLE
    );
}


static void test_capable_3d_runs_every_method(void)
{
    assert(
        localization_method_availability(
            LOCALIZATION_DIMENSIONALITY_CAPABLE_3D,
            LOCALIZATION_METHOD_REQUIRES_3D
        ) == LOCALIZATION_METHOD_RUNNABLE
    );
    assert(
        localization_method_availability(
            LOCALIZATION_DIMENSIONALITY_CAPABLE_3D,
            LOCALIZATION_METHOD_SUPPORTS_2D_AND_3D
        ) == LOCALIZATION_METHOD_RUNNABLE
    );
}


static void test_reasons_are_non_empty_and_distinct_for_skips_only(void)
{
    const char *runnable = localization_method_availability_reason(LOCALIZATION_METHOD_RUNNABLE);
    const char *insufficient = localization_method_availability_reason(
        LOCALIZATION_METHOD_SKIPPED_INSUFFICIENT_SENSORS
    );
    const char *requires_3d = localization_method_availability_reason(
        LOCALIZATION_METHOD_SKIPPED_REQUIRES_3D
    );

    assert(runnable != NULL);
    assert(strlen(runnable) == 0u);
    assert(insufficient != NULL && strlen(insufficient) > 0u);
    assert(requires_3d != NULL && strlen(requires_3d) > 0u);
    assert(strcmp(insufficient, requires_3d) != 0);
}


int main(void)
{
    test_classify_boundaries();
    test_insufficient_blocks_every_method_regardless_of_requirement();
    test_planar_2d_skips_3d_only_methods_but_runs_flexible_ones();
    test_capable_3d_runs_every_method();
    test_reasons_are_non_empty_and_distinct_for_skips_only();
    puts("localization dimensionality policy tests passed");
    return 0;
}
