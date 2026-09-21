#include <assert.h>
#include <string.h>

#include "app_pod_status_local_position_policy.h"

static object_store_pod_geometry_t make_pod(
    bool computed,
    int32_t x_mm,
    int32_t y_mm)
{
    object_store_pod_geometry_t pod;
    memset(&pod, 0, sizeof(pod));
    pod.computed = computed;
    pod.x_mm = x_mm;
    pod.y_mm = y_mm;
    return pod;
}

static void test_null_pod_is_invalid(void)
{
    const app_pod_status_local_position_reading_t reading =
        app_pod_status_bridge_compute_local_position_reading(NULL);
    assert(!reading.valid);
    assert(reading.local_x_m == 0.0f);
    assert(reading.local_y_m == 0.0f);
}

static void test_unsolved_pod_is_invalid(void)
{
    /* computed == false: no path of valid CS pairs back to MAIN this cycle.
     * x_mm/y_mm are left at whatever stale value the last solve wrote (here
     * deliberately nonzero) to prove the reading is rejected on `computed`
     * alone, not inferred from the coordinates looking "empty". */
    const object_store_pod_geometry_t pod = make_pod(false, 4200, -1300);
    const app_pod_status_local_position_reading_t reading =
        app_pod_status_bridge_compute_local_position_reading(&pod);
    assert(!reading.valid);
    assert(reading.local_x_m == 0.0f);
    assert(reading.local_y_m == 0.0f);
}

static void test_solved_pod_converts_millimeters_to_meters(void)
{
    const object_store_pod_geometry_t pod = make_pod(true, 3200, -450);
    const app_pod_status_local_position_reading_t reading =
        app_pod_status_bridge_compute_local_position_reading(&pod);
    assert(reading.valid);
    assert(reading.local_x_m == 3.2f);
    assert(reading.local_y_m == -0.45f);
}

static void test_solved_origin_pod_is_still_valid(void)
{
    /* (0, 0) is a legitimate solved position (e.g. exactly on the MAIN-A
     * axis), not to be confused with an unsolved/zeroed default. */
    const object_store_pod_geometry_t pod = make_pod(true, 0, 0);
    const app_pod_status_local_position_reading_t reading =
        app_pod_status_bridge_compute_local_position_reading(&pod);
    assert(reading.valid);
    assert(reading.local_x_m == 0.0f);
    assert(reading.local_y_m == 0.0f);
}

int main(void)
{
    test_null_pod_is_invalid();
    test_unsolved_pod_is_invalid();
    test_solved_pod_converts_millimeters_to_meters();
    test_solved_origin_pod_is_still_valid();
    return 0;
}
