#include <assert.h>
#include <string.h>

#include "nrf_emergency_stop_policy.h"


static nrf_emergency_stop_channel_sample_t make_sample(
    int32_t altitude_mm,
    int32_t ax,
    int32_t ay,
    int32_t az
)
{
    nrf_emergency_stop_channel_sample_t sample = {0};
    sample.valid = true;
    sample.altitude_mm = altitude_mm;
    sample.accel_mg[0] = ax;
    sample.accel_mg[1] = ay;
    sample.accel_mg[2] = az;
    return sample;
}


static void test_invalid_samples_never_trigger(void)
{
    nrf_emergency_stop_channel_sample_t baseline = make_sample(0, 0, 0, 1000);
    nrf_emergency_stop_channel_sample_t current = make_sample(5000, 5000, 5000, 5000);
    current.valid = false;
    assert(!nrf_emergency_stop_channel_triggered(&baseline, &current));

    baseline.valid = false;
    current.valid = true;
    assert(!nrf_emergency_stop_channel_triggered(&baseline, &current));
}


static void test_small_single_axis_drift_does_not_trigger(void)
{
    /* Geophone resting on solid ground: noise-level drift on one axis only. */
    nrf_emergency_stop_channel_sample_t baseline = make_sample(1500, 10, -20, 990);
    nrf_emergency_stop_channel_sample_t current = make_sample(1520, 60, -20, 990);
    assert(!nrf_emergency_stop_channel_triggered(&baseline, &current));
}


static void test_altitude_jump_over_one_meter_triggers(void)
{
    nrf_emergency_stop_channel_sample_t baseline = make_sample(1500, 0, 0, 1000);
    nrf_emergency_stop_channel_sample_t current = make_sample(2501, 0, 0, 1000);
    assert(nrf_emergency_stop_channel_triggered(&baseline, &current));

    /* Exactly at the threshold must not trigger -- only strictly over. */
    nrf_emergency_stop_channel_sample_t at_threshold = make_sample(2500, 0, 0, 1000);
    assert(!nrf_emergency_stop_channel_triggered(&baseline, &at_threshold));
}


static void test_altitude_drop_over_one_meter_triggers(void)
{
    nrf_emergency_stop_channel_sample_t baseline = make_sample(1500, 0, 0, 1000);
    nrf_emergency_stop_channel_sample_t current = make_sample(499, 0, 0, 1000);
    assert(nrf_emergency_stop_channel_triggered(&baseline, &current));
}


static void test_two_axis_shift_does_not_trigger(void)
{
    nrf_emergency_stop_channel_sample_t baseline = make_sample(1500, 0, 0, 1000);
    /* Two axes shift well past threshold, third stays put -- must not trigger. */
    nrf_emergency_stop_channel_sample_t current = make_sample(1500, 800, 800, 1000);
    assert(!nrf_emergency_stop_channel_triggered(&baseline, &current));
}


static void test_all_three_axes_shift_triggers(void)
{
    nrf_emergency_stop_channel_sample_t baseline = make_sample(1500, 0, 0, 1000);
    nrf_emergency_stop_channel_sample_t current = make_sample(1500, 800, -800, 400);
    assert(nrf_emergency_stop_channel_triggered(&baseline, &current));
}


static void test_any_channel_reports_first_trigger_index(void)
{
    nrf_emergency_stop_channel_sample_t baseline[3];
    nrf_emergency_stop_channel_sample_t current[3];
    for (size_t i = 0U; i < 3U; ++i) {
        baseline[i] = make_sample(1500, 0, 0, 1000);
        current[i] = baseline[i];
    }
    /* Channel 2 is the one that fell. */
    current[2] = make_sample(1500, 800, -800, 400);

    size_t triggered = 999U;
    assert(nrf_emergency_stop_any_channel_triggered(baseline, current, 3U, &triggered));
    assert(triggered == 2U);
}


static void test_any_channel_false_when_none_trigger(void)
{
    nrf_emergency_stop_channel_sample_t baseline[2];
    nrf_emergency_stop_channel_sample_t current[2];
    for (size_t i = 0U; i < 2U; ++i) {
        baseline[i] = make_sample(1500, 0, 0, 1000);
        current[i] = baseline[i];
    }
    assert(!nrf_emergency_stop_any_channel_triggered(baseline, current, 2U, NULL));
}


int main(void)
{
    test_invalid_samples_never_trigger();
    test_small_single_axis_drift_does_not_trigger();
    test_altitude_jump_over_one_meter_triggers();
    test_altitude_drop_over_one_meter_triggers();
    test_two_axis_shift_does_not_trigger();
    test_all_three_axes_shift_triggers();
    test_any_channel_reports_first_trigger_index();
    test_any_channel_false_when_none_trigger();
    return 0;
}
