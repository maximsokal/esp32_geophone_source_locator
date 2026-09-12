#include "unity.h"

#include "ad7606_presence_policy.h"
#include "localization.h"
#include "object_format.h"
#include "ui_measurement_trace.h"


TEST_CASE(
    "object IDs accept the storage format and reject path fragments",
    "[object_format]"
)
{
    TEST_ASSERT_TRUE(object_format_id_is_valid("OBJ_0042"));
    TEST_ASSERT_FALSE(object_format_id_is_valid("OBJ_42"));
    TEST_ASSERT_FALSE(object_format_id_is_valid("OBJ_0042/.."));
}


TEST_CASE(
    "AD7606 presence requires complete BUSY pulses",
    "[ad7606]"
)
{
    ad7606_presence_observer_t observer;
    ad7606_presence_observer_reset(&observer, 0);

    ad7606_presence_observer_observe(&observer, 1);
    ad7606_presence_observer_observe(&observer, 0);
    ad7606_presence_observer_observe(&observer, 1);
    ad7606_presence_observer_observe(&observer, 0);
    TEST_ASSERT_FALSE(ad7606_presence_observer_ready(&observer));

    ad7606_presence_observer_observe(&observer, 1);
    ad7606_presence_observer_observe(&observer, 0);
    TEST_ASSERT_TRUE(ad7606_presence_observer_ready(&observer));
    TEST_ASSERT_EQUAL_UINT32(3U, observer.completed_pulses);
}


TEST_CASE(
    "trace reduction keeps a DC-removed min max envelope",
    "[ui]"
)
{
    const seismic_frame_t frames[] = {
        {.timestamp_us = 1, .channel_uv = {1.0f, -2.0f, 3.0f, 4.0f}},
        {.timestamp_us = 2, .channel_uv = {2.0f, -9.0f, 1.0f, 5.0f}},
    };
    int32_t trace_min[SEISMIC_CHANNEL_COUNT][UI_MEASUREMENT_TRACE_POINTS];
    int32_t trace_max[SEISMIC_CHANNEL_COUNT][UI_MEASUREMENT_TRACE_POINTS];

    const float maximum = ui_measurement_trace_reduce(
        frames,
        sizeof(frames) / sizeof(frames[0]),
        trace_min,
        trace_max
    );

    TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.5f, maximum);
    TEST_ASSERT_EQUAL_INT(4, trace_min[1][UI_MEASUREMENT_TRACE_POINTS - 2U]);
    TEST_ASSERT_EQUAL_INT(4, trace_max[1][UI_MEASUREMENT_TRACE_POINTS - 2U]);
    TEST_ASSERT_EQUAL_INT(-4, trace_min[1][UI_MEASUREMENT_TRACE_POINTS - 1U]);
    TEST_ASSERT_EQUAL_INT(-4, trace_max[1][UI_MEASUREMENT_TRACE_POINTS - 1U]);
}


TEST_CASE(
    "localization rejects a coplanar sensor geometry",
    "[localization]"
)
{
    const localization_observation_t observations[] = {
        {{0.0, 0.0, 0.0}, 0},
        {{1.0, 0.0, 0.0}, 0},
        {{0.0, 1.0, 0.0}, 0},
        {{1.0, 1.0, 0.0}, 0},
    };
    localization_config_t config = localization_default_config();
    config.propagation_speed_mps = 1000.0;
    localization_result_t result;

    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_STATE,
        localization_tdoa_solve(
            observations,
            sizeof(observations) / sizeof(observations[0]),
            &config,
            NULL,
            &result
        )
    );
}
