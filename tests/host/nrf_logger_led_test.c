#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "nrf_logger_led.h"
#include "nrf_sensor_snapshot.h"

static nrf_logger_led_input_t ready_remote_input(void)
{
    nrf_logger_led_input_t in = {0};
    in.node_state.role = NRF_NODE_ROLE_REMOTE;
    in.node_state.lifecycle = NRF_NODE_LIFECYCLE_READY;
    in.bme280_ready = true;
    return in;
}

static void assert_output_invariants(const nrf_logger_led_t *led)
{
    int active_count = (led->output.green ? 1 : 0) +
        (led->output.yellow ? 1 : 0) + (led->output.red ? 1 : 0);

    assert(active_count <= 1);
    if (led->output.green) {
        assert(led->output.severity_latched == NRF_LOGGER_LED_SEVERITY_NONE);
    }
    if (led->output.yellow) {
        assert(led->output.severity_latched == NRF_LOGGER_LED_SEVERITY_WARNING);
    }
    if (led->output.red) {
        assert(led->output.severity_latched >= NRF_LOGGER_LED_SEVERITY_ERROR);
    }
}

static void update_ok(
    nrf_logger_led_t *led,
    const nrf_logger_led_input_t *in,
    uint32_t now_ms)
{
    assert(nrf_logger_led_update(led, in, now_ms) == NRF_LOGGER_LED_OK);
    assert_output_invariants(led);
}

static nrf_logger_led_state_t ready_remote_state(void)
{
    nrf_logger_led_state_t state = {0};
    state.node_state.role = NRF_NODE_ROLE_REMOTE;
    state.node_state.lifecycle = NRF_NODE_LIFECYCLE_READY;
    state.bmi160_ready = true;
    state.bme280_ready = true;
    return state;
}

static void map_state_ok(
    const nrf_logger_led_state_t *state,
    nrf_logger_led_input_t *input)
{
    assert(nrf_logger_led_inputs_from_state(state, input) == NRF_LOGGER_LED_OK);
}

static void test_persistent_runtime_state_mapping(void)
{
    nrf_logger_led_state_t state = ready_remote_state();
    nrf_logger_led_input_t input;
    nrf_logger_led_t led;

    map_state_ok(&state, &input);
    assert(input.node_state.role == NRF_NODE_ROLE_REMOTE);
    assert(input.node_state.lifecycle == NRF_NODE_LIFECYCLE_READY);
    assert(input.bme280_ready);
    assert(!input.bmi160_init_failed);
    assert(!input.bme280_init_failed);
    for (uint32_t source = 0U;
         source < (uint32_t)NRF_LOGGER_LED_SOURCE_COUNT;
         ++source) {
        assert(input.source_levels[source] == NRF_LOGGER_LED_SEVERITY_NONE);
    }

    state.bmi160_ready = false;
    map_state_ok(&state, &input);
    assert(input.source_levels[NRF_LOGGER_LED_SOURCE_BMI160_FAULT] ==
           NRF_LOGGER_LED_SEVERITY_NONE);
    state.bmi160_init_failed = true;
    map_state_ok(&state, &input);
    assert(input.bmi160_init_failed);
    nrf_logger_led_reset(&led);
    update_ok(&led, &input, 0U);
    assert(led.output.red);
    assert(led.output.reason_source == NRF_LOGGER_LED_SOURCE_BMI160_FAULT);

    state = ready_remote_state();
    state.sensor_fault_mask = NRF_SENSOR_VALID_IMU;
    map_state_ok(&state, &input);
    assert(input.source_levels[NRF_LOGGER_LED_SOURCE_BMI160_FAULT] ==
           NRF_LOGGER_LED_SEVERITY_ERROR);
    nrf_logger_led_reset(&led);
    update_ok(&led, &input, 1U);
    assert(led.output.red);
    assert(led.output.reason_source == NRF_LOGGER_LED_SOURCE_BMI160_FAULT);

    state = ready_remote_state();
    state.bme280_ready = false;
    map_state_ok(&state, &input);
    assert(!input.bme280_ready);
    assert(!input.bme280_init_failed);
    assert(input.source_levels[NRF_LOGGER_LED_SOURCE_BME280_FAULT] ==
           NRF_LOGGER_LED_SEVERITY_NONE);
    state.bme280_init_failed = true;
    map_state_ok(&state, &input);
    assert(input.bme280_init_failed);
    assert(input.source_levels[NRF_LOGGER_LED_SOURCE_BME280_FAULT] ==
           NRF_LOGGER_LED_SEVERITY_NONE);
    nrf_logger_led_reset(&led);
    update_ok(&led, &input, 2U);
    assert(led.output.red);
    assert(led.output.reason_source == NRF_LOGGER_LED_SOURCE_BME280_FAULT);
    state = ready_remote_state();
    state.sensor_fault_mask = NRF_SENSOR_VALID_ENV;
    map_state_ok(&state, &input);
    assert(input.source_levels[NRF_LOGGER_LED_SOURCE_BME280_FAULT] ==
           NRF_LOGGER_LED_SEVERITY_ERROR);
    nrf_logger_led_reset(&led);
    update_ok(&led, &input, 3U);
    assert(led.output.red);
    assert(led.output.reason_source == NRF_LOGGER_LED_SOURCE_BME280_FAULT);

    state = ready_remote_state();
    map_state_ok(&state, &input);
    assert(input.source_levels[NRF_LOGGER_LED_SOURCE_LOG_RING_LOSS] ==
           NRF_LOGGER_LED_SEVERITY_NONE);
    state.log_overwritten_total = 1U;
    map_state_ok(&state, &input);
    assert(input.source_levels[NRF_LOGGER_LED_SOURCE_LOG_RING_LOSS] ==
           NRF_LOGGER_LED_SEVERITY_WARNING);
    state.log_overwritten_total = 2U;
    state.log_lost_internal_total = 1U;
    map_state_ok(&state, &input);
    assert(input.source_levels[NRF_LOGGER_LED_SOURCE_LOG_RING_LOSS] ==
           NRF_LOGGER_LED_SEVERITY_WARNING);
    nrf_logger_led_reset(&led);
    update_ok(&led, &input, 0U);
    assert(led.output.green);
    update_ok(&led, &input, 1999U);
    assert(led.output.green);
    update_ok(&led, &input, 2000U);
    assert(led.output.yellow);

    state = ready_remote_state();
    state.watchdog_participant_stale_total = 1U;
    map_state_ok(&state, &input);
    nrf_logger_led_reset(&led);
    update_ok(&led, &input, 7U);
    assert(led.output.red);
    assert(led.output.severity_latched == NRF_LOGGER_LED_SEVERITY_FATAL);

    state = ready_remote_state();
    state.twis_protocol_error_total = 1U;
    state.cs_ranging_failure_total = 1U;
    state.role_lost_total = 1U;
    state.watchdog_participant_stale_total = 1U;
    state.init_boot_fatal = true;
    map_state_ok(&state, &input);
    assert(input.source_levels[NRF_LOGGER_LED_SOURCE_TWIS_PROTOCOL] ==
           NRF_LOGGER_LED_SEVERITY_WARNING);
    assert(input.source_levels[NRF_LOGGER_LED_SOURCE_CS_RANGING] ==
           NRF_LOGGER_LED_SEVERITY_ERROR);
    assert(input.source_levels[NRF_LOGGER_LED_SOURCE_ROLE_LOST] ==
           NRF_LOGGER_LED_SEVERITY_WARNING);
    assert(input.source_levels[NRF_LOGGER_LED_SOURCE_WATCHDOG_PARTICIPANT_STALE] ==
           NRF_LOGGER_LED_SEVERITY_FATAL);
    assert(input.source_levels[NRF_LOGGER_LED_SOURCE_INIT_BOOT_FATAL] ==
           NRF_LOGGER_LED_SEVERITY_FATAL);

    nrf_logger_led_reset(&led);
    update_ok(&led, &input, 0U);
    assert(led.output.red);
    assert(led.output.severity_latched == NRF_LOGGER_LED_SEVERITY_FATAL);

    state = ready_remote_state();
    state.node_state.role = NRF_NODE_ROLE_MAIN;
    state.init_boot_fatal = true;
    map_state_ok(&state, &input);
    nrf_logger_led_reset(&led);
    update_ok(&led, &input, 1U);
    assert(led.output.red);
    assert(led.output.severity_latched == NRF_LOGGER_LED_SEVERITY_FATAL);

    assert(nrf_logger_led_inputs_from_state(NULL, &input) ==
           NRF_LOGGER_LED_ERR_ARGUMENT);
    assert(nrf_logger_led_inputs_from_state(&state, NULL) ==
           NRF_LOGGER_LED_ERR_ARGUMENT);
}

static void test_remote_ready_truth_table_for_each_source_and_level(void)
{
    const nrf_logger_led_severity_t levels[] = {
        NRF_LOGGER_LED_SEVERITY_NONE,
        NRF_LOGGER_LED_SEVERITY_WARNING,
        NRF_LOGGER_LED_SEVERITY_ERROR,
        NRF_LOGGER_LED_SEVERITY_FATAL,
    };

    for (uint32_t source = 0U;
         source < (uint32_t)NRF_LOGGER_LED_SOURCE_COUNT;
         ++source) {
        for (size_t level_index = 0U;
             level_index < sizeof(levels) / sizeof(levels[0]);
             ++level_index) {
            nrf_logger_led_t led;
            nrf_logger_led_input_t in = ready_remote_input();
            nrf_logger_led_reset(&led);
            in.source_levels[source] = levels[level_index];

            update_ok(&led, &in, 0U);
            if (levels[level_index] == NRF_LOGGER_LED_SEVERITY_WARNING) {
                assert(led.output.green);
                assert(led.output.severity_latched == NRF_LOGGER_LED_SEVERITY_NONE);
                update_ok(&led, &in, NRF_LOGGER_LED_WARNING_CONFIRM_MS);
            }

            if (levels[level_index] == NRF_LOGGER_LED_SEVERITY_NONE) {
                assert(led.output.green);
                assert(led.output.severity_latched == NRF_LOGGER_LED_SEVERITY_NONE);
                assert(led.output.reason_source == NRF_LOGGER_LED_SOURCE_NONE);
            } else if (levels[level_index] == NRF_LOGGER_LED_SEVERITY_WARNING) {
                assert(led.output.yellow);
                assert(led.output.severity_latched == NRF_LOGGER_LED_SEVERITY_WARNING);
                assert(led.output.reason_source == (nrf_logger_led_source_t)source);
            } else {
                assert(led.output.red);
                assert(led.output.severity_latched == levels[level_index]);
                assert(led.output.reason_source == (nrf_logger_led_source_t)source);
            }
        }
    }
}

static void test_warning_requires_full_confirmation_and_flaps_restart(void)
{
    nrf_logger_led_t led;
    nrf_logger_led_input_t in = ready_remote_input();
    nrf_logger_led_reset(&led);
    in.source_levels[NRF_LOGGER_LED_SOURCE_ROLE_LOST] =
        NRF_LOGGER_LED_SEVERITY_WARNING;

    update_ok(&led, &in, 0U);
    assert(led.output.green);
    update_ok(&led, &in, 1999U);
    assert(led.output.green);
    assert(led.output.severity_latched == NRF_LOGGER_LED_SEVERITY_NONE);
    update_ok(&led, &in, 2000U);
    assert(led.output.yellow);
    assert(led.output.severity_latched == NRF_LOGGER_LED_SEVERITY_WARNING);

    nrf_logger_led_reset(&led);
    in = ready_remote_input();
    in.source_levels[NRF_LOGGER_LED_SOURCE_ROLE_LOST] =
        NRF_LOGGER_LED_SEVERITY_WARNING;
    update_ok(&led, &in, 0U);
    in.source_levels[NRF_LOGGER_LED_SOURCE_ROLE_LOST] =
        NRF_LOGGER_LED_SEVERITY_NONE;
    update_ok(&led, &in, 1999U);
    assert(led.output.green);
    assert(led.output.severity_latched == NRF_LOGGER_LED_SEVERITY_NONE);

    in.source_levels[NRF_LOGGER_LED_SOURCE_ROLE_LOST] =
        NRF_LOGGER_LED_SEVERITY_WARNING;
    update_ok(&led, &in, 2000U);
    update_ok(&led, &in, 3998U);
    in.source_levels[NRF_LOGGER_LED_SOURCE_ROLE_LOST] =
        NRF_LOGGER_LED_SEVERITY_NONE;
    update_ok(&led, &in, 3999U);
    assert(led.output.green);
    assert(led.output.severity_latched == NRF_LOGGER_LED_SEVERITY_NONE);

    in.source_levels[NRF_LOGGER_LED_SOURCE_ROLE_LOST] =
        NRF_LOGGER_LED_SEVERITY_WARNING;
    update_ok(&led, &in, 4000U);
    update_ok(&led, &in, 5999U);
    assert(led.output.green);
    assert(led.output.severity_latched == NRF_LOGGER_LED_SEVERITY_NONE);
    in.source_levels[NRF_LOGGER_LED_SOURCE_ROLE_LOST] =
        NRF_LOGGER_LED_SEVERITY_NONE;
    update_ok(&led, &in, 6000U);
    assert(led.output.green);
    assert(led.output.severity_latched == NRF_LOGGER_LED_SEVERITY_NONE);
}

static void test_errors_and_fatals_latch_immediately(void)
{
    nrf_logger_led_t led;
    nrf_logger_led_input_t in = ready_remote_input();
    nrf_logger_led_reset(&led);
    in.source_levels[NRF_LOGGER_LED_SOURCE_TWIS_PROTOCOL] =
        NRF_LOGGER_LED_SEVERITY_ERROR;
    update_ok(&led, &in, 123U);
    assert(led.output.red);
    assert(led.output.severity_latched == NRF_LOGGER_LED_SEVERITY_ERROR);

    nrf_logger_led_reset(&led);
    in = ready_remote_input();
    in.source_levels[NRF_LOGGER_LED_SOURCE_WATCHDOG_PARTICIPANT_STALE] =
        NRF_LOGGER_LED_SEVERITY_FATAL;
    update_ok(&led, &in, 900U);
    assert(led.output.red);
    assert(led.output.severity_latched == NRF_LOGGER_LED_SEVERITY_FATAL);
    assert(led.output.reason_source ==
           NRF_LOGGER_LED_SOURCE_WATCHDOG_PARTICIPANT_STALE);

    nrf_logger_led_reset(&led);
    in = ready_remote_input();
    in.source_levels[NRF_LOGGER_LED_SOURCE_INIT_BOOT_FATAL] =
        NRF_LOGGER_LED_SEVERITY_FATAL;
    update_ok(&led, &in, 1700U);
    assert(led.output.red);
    assert(led.output.severity_latched == NRF_LOGGER_LED_SEVERITY_FATAL);
    assert(led.output.reason_source == NRF_LOGGER_LED_SOURCE_INIT_BOOT_FATAL);
}

static void test_bme280_is_required_after_remote_ready(void)
{
    nrf_logger_led_t led;
    nrf_logger_led_input_t in = ready_remote_input();
    nrf_logger_led_reset(&led);
    in.bme280_ready = false;

    update_ok(&led, &in, 1U);
    assert(led.output.green);
    assert(led.output.severity_latched == NRF_LOGGER_LED_SEVERITY_NONE);

    nrf_logger_led_reset(&led);
    in.bme280_init_failed = true;
    update_ok(&led, &in, 2U);
    assert(led.output.red);
    assert(led.output.severity_latched == NRF_LOGGER_LED_SEVERITY_ERROR);
    assert(led.output.reason_source == NRF_LOGGER_LED_SOURCE_BME280_FAULT);
}

static void test_pending_remote_sensor_preparation_does_not_latch(void)
{
    nrf_logger_led_state_t state = ready_remote_state();
    nrf_logger_led_input_t input;
    nrf_logger_led_t led;

    state.bmi160_ready = false;
    state.bme280_ready = false;
    state.sensor_fault_mask = NRF_SENSOR_VALID_IMU | NRF_SENSOR_VALID_ENV;
    map_state_ok(&state, &input);
    assert(!input.bmi160_init_failed);
    assert(!input.bme280_init_failed);
    assert(input.source_levels[NRF_LOGGER_LED_SOURCE_BMI160_FAULT] ==
           NRF_LOGGER_LED_SEVERITY_NONE);
    assert(input.source_levels[NRF_LOGGER_LED_SOURCE_BME280_FAULT] ==
           NRF_LOGGER_LED_SEVERITY_NONE);

    nrf_logger_led_reset(&led);
    for (uint32_t update = 0U; update < 32U; ++update) {
        update_ok(&led, &input, update + 1U);
        assert(led.output.green);
        assert(led.output.severity_latched == NRF_LOGGER_LED_SEVERITY_NONE);
    }
}

static void test_sensor_retry_does_not_clear_error_latch(void)
{
    nrf_logger_led_t led;
    nrf_logger_led_input_t in = ready_remote_input();

    nrf_logger_led_reset(&led);
    in.bme280_ready = false;
    in.bme280_init_failed = true;
    update_ok(&led, &in, 1U);
    assert(led.output.red);

    in.bme280_ready = true;
    in.bme280_init_failed = false;
    update_ok(&led, &in, 2U);
    assert(led.output.red);
    assert(led.output.reason_source == NRF_LOGGER_LED_SOURCE_BME280_FAULT);

    nrf_logger_led_reset(&led);
    update_ok(&led, &in, 3U);
    assert(led.output.green);
}

static void test_red_suppresses_yellow_when_warning_and_error_are_latched(void)
{
    nrf_logger_led_t led;
    nrf_logger_led_input_t in = ready_remote_input();
    nrf_logger_led_reset(&led);
    in.source_levels[NRF_LOGGER_LED_SOURCE_ROLE_LOST] =
        NRF_LOGGER_LED_SEVERITY_WARNING;
    update_ok(&led, &in, 0U);
    update_ok(&led, &in, NRF_LOGGER_LED_WARNING_CONFIRM_MS);
    assert(led.output.yellow);

    in.source_levels[NRF_LOGGER_LED_SOURCE_ROLE_LOST] =
        NRF_LOGGER_LED_SEVERITY_NONE;
    update_ok(&led, &in, 2001U);
    assert(led.output.yellow);
    in.source_levels[NRF_LOGGER_LED_SOURCE_LOG_RING_LOSS] =
        NRF_LOGGER_LED_SEVERITY_ERROR;
    update_ok(&led, &in, 2002U);
    assert(led.output.red);
    assert(!led.output.yellow);
    assert(!led.output.green);
    assert(led.output.severity_latched == NRF_LOGGER_LED_SEVERITY_ERROR);
}

static void test_reason_changes_only_when_latched_severity_rises(void)
{
    nrf_logger_led_t led;
    nrf_logger_led_input_t in = ready_remote_input();
    nrf_logger_led_reset(&led);
    in.source_levels[NRF_LOGGER_LED_SOURCE_BMI160_FAULT] =
        NRF_LOGGER_LED_SEVERITY_WARNING;
    update_ok(&led, &in, 0U);
    update_ok(&led, &in, 2000U);
    assert(led.output.reason_source == NRF_LOGGER_LED_SOURCE_BMI160_FAULT);

    in.source_levels[NRF_LOGGER_LED_SOURCE_TWIS_PROTOCOL] =
        NRF_LOGGER_LED_SEVERITY_ERROR;
    update_ok(&led, &in, 2001U);
    assert(led.output.severity_latched == NRF_LOGGER_LED_SEVERITY_ERROR);
    assert(led.output.reason_source == NRF_LOGGER_LED_SOURCE_TWIS_PROTOCOL);

    in.source_levels[NRF_LOGGER_LED_SOURCE_LOG_RING_LOSS] =
        NRF_LOGGER_LED_SEVERITY_ERROR;
    update_ok(&led, &in, 2002U);
    assert(led.output.reason_source == NRF_LOGGER_LED_SOURCE_TWIS_PROTOCOL);
    in.source_levels[NRF_LOGGER_LED_SOURCE_INIT_BOOT_FATAL] =
        NRF_LOGGER_LED_SEVERITY_FATAL;
    update_ok(&led, &in, 2003U);
    assert(led.output.severity_latched == NRF_LOGGER_LED_SEVERITY_FATAL);
    assert(led.output.reason_source == NRF_LOGGER_LED_SOURCE_INIT_BOOT_FATAL);

    in.source_levels[NRF_LOGGER_LED_SOURCE_WATCHDOG_PARTICIPANT_STALE] =
        NRF_LOGGER_LED_SEVERITY_FATAL;
    update_ok(&led, &in, 2004U);
    assert(led.output.reason_source == NRF_LOGGER_LED_SOURCE_INIT_BOOT_FATAL);
}

static void test_latch_persists_until_reset(void)
{
    nrf_logger_led_t led;
    nrf_logger_led_input_t in = ready_remote_input();
    nrf_logger_led_reset(&led);
    in.source_levels[NRF_LOGGER_LED_SOURCE_CS_RANGING] =
        NRF_LOGGER_LED_SEVERITY_WARNING;
    update_ok(&led, &in, 10U);
    update_ok(&led, &in, 2010U);
    assert(led.output.yellow);

    in.source_levels[NRF_LOGGER_LED_SOURCE_CS_RANGING] =
        NRF_LOGGER_LED_SEVERITY_NONE;
    update_ok(&led, &in, 2011U);
    assert(led.output.yellow);
    assert(led.output.severity_latched == NRF_LOGGER_LED_SEVERITY_WARNING);

    nrf_logger_led_reset(&led);
    assert(led.output.severity_latched == NRF_LOGGER_LED_SEVERITY_NONE);
    assert(led.output.reason_source == NRF_LOGGER_LED_SOURCE_NONE);
    assert(!led.output.green && !led.output.yellow && !led.output.red);
    update_ok(&led, &in, 2012U);
    assert(led.output.green);
}

static void test_main_uses_common_health_without_remote_sensors(void)
{
    nrf_logger_led_t led;
    nrf_logger_led_input_t in = ready_remote_input();
    nrf_logger_led_reset(&led);
    in.node_state.role = NRF_NODE_ROLE_MAIN;
    in.bmi160_init_failed = true;
    in.bme280_init_failed = true;
    in.source_levels[NRF_LOGGER_LED_SOURCE_BMI160_FAULT] = NRF_LOGGER_LED_SEVERITY_ERROR;
    in.source_levels[NRF_LOGGER_LED_SOURCE_BME280_FAULT] = NRF_LOGGER_LED_SEVERITY_ERROR;
    update_ok(&led, &in, 0U);
    assert(led.output.green);
    assert(led.output.severity_latched == NRF_LOGGER_LED_SEVERITY_NONE);

    in.source_levels[NRF_LOGGER_LED_SOURCE_TWIS_PROTOCOL] = NRF_LOGGER_LED_SEVERITY_WARNING;
    update_ok(&led, &in, 1U);
    update_ok(&led, &in, NRF_LOGGER_LED_WARNING_CONFIRM_MS);
    assert(led.output.green);
    update_ok(&led, &in, NRF_LOGGER_LED_WARNING_CONFIRM_MS + 1U);
    assert(led.output.yellow);
    in.source_levels[NRF_LOGGER_LED_SOURCE_CS_RANGING] = NRF_LOGGER_LED_SEVERITY_ERROR;
    update_ok(&led, &in, NRF_LOGGER_LED_WARNING_CONFIRM_MS + 2U);
    assert(led.output.red && !led.output.yellow);
    in.source_levels[NRF_LOGGER_LED_SOURCE_CS_RANGING] = NRF_LOGGER_LED_SEVERITY_NONE;
    update_ok(&led, &in, NRF_LOGGER_LED_WARNING_CONFIRM_MS + 3U);
    assert(led.output.red);
    in.source_levels[NRF_LOGGER_LED_SOURCE_INIT_BOOT_FATAL] = NRF_LOGGER_LED_SEVERITY_FATAL;
    update_ok(&led, &in, NRF_LOGGER_LED_WARNING_CONFIRM_MS + 4U);
    assert(led.output.reason_source == NRF_LOGGER_LED_SOURCE_INIT_BOOT_FATAL);
    in.node_state.lifecycle = NRF_NODE_LIFECYCLE_UNAVAILABLE;
    update_ok(&led, &in, NRF_LOGGER_LED_WARNING_CONFIRM_MS + 5U);
    assert(!led.output.green && !led.output.yellow && !led.output.red);
    in.node_state.lifecycle = NRF_NODE_LIFECYCLE_READY;
    update_ok(&led, &in, NRF_LOGGER_LED_WARNING_CONFIRM_MS + 6U);
    assert(led.output.red);
}

static void test_unassigned_and_unknown_roles_fail_off(void)
{
    const nrf_node_role_t roles[] = {
        NRF_NODE_ROLE_UNASSIGNED,
        (nrf_node_role_t)99,
    };

    for (size_t role_index = 0U;
         role_index < sizeof(roles) / sizeof(roles[0]);
         ++role_index) {
        nrf_logger_led_t led;
        nrf_logger_led_input_t in = ready_remote_input();
        nrf_logger_led_reset(&led);
        in.node_state.role = roles[role_index];
        in.source_levels[NRF_LOGGER_LED_SOURCE_INIT_BOOT_FATAL] =
            NRF_LOGGER_LED_SEVERITY_FATAL;
        in.source_levels[NRF_LOGGER_LED_SOURCE_ROLE_LOST] =
            NRF_LOGGER_LED_SEVERITY_WARNING;
        update_ok(&led, &in, 0U);
        assert(!led.output.green && !led.output.yellow && !led.output.red);
        assert(led.output.severity_latched == NRF_LOGGER_LED_SEVERITY_NONE);
        assert(led.output.reason_source == NRF_LOGGER_LED_SOURCE_NONE);
    }
}

static void test_non_ready_remote_never_shows_green(void)
{
    nrf_logger_led_t led;
    nrf_logger_led_input_t in = ready_remote_input();
    nrf_logger_led_reset(&led);
    in.node_state.lifecycle = NRF_NODE_LIFECYCLE_BOOTING;
    update_ok(&led, &in, 0U);
    assert(!led.output.green && !led.output.yellow && !led.output.red);

    in.node_state.lifecycle = NRF_NODE_LIFECYCLE_UNAVAILABLE;
    update_ok(&led, &in, 1U);
    assert(!led.output.green && !led.output.yellow && !led.output.red);

    in.node_state.lifecycle = NRF_NODE_LIFECYCLE_BOOTING;
    in.source_levels[NRF_LOGGER_LED_SOURCE_INIT_BOOT_FATAL] =
        NRF_LOGGER_LED_SEVERITY_FATAL;
    update_ok(&led, &in, 2U);
    assert(!led.output.green && !led.output.yellow && !led.output.red);
    assert(led.output.severity_latched == NRF_LOGGER_LED_SEVERITY_FATAL);

    in.node_state.lifecycle = NRF_NODE_LIFECYCLE_READY;
    in.source_levels[NRF_LOGGER_LED_SOURCE_INIT_BOOT_FATAL] =
        NRF_LOGGER_LED_SEVERITY_NONE;
    update_ok(&led, &in, 3U);
    assert(led.output.red);
}

static void test_invalid_source_levels_are_errors_and_counted(void)
{
    nrf_logger_led_t led;
    nrf_logger_led_input_t in = ready_remote_input();
    nrf_logger_led_reset(&led);
    in.source_levels[NRF_LOGGER_LED_SOURCE_BMI160_FAULT] =
        (nrf_logger_led_severity_t)99;
    in.source_levels[NRF_LOGGER_LED_SOURCE_BME280_FAULT] =
        (nrf_logger_led_severity_t)-1;

    update_ok(&led, &in, 10U);
    assert(led.output.red);
    assert(led.output.severity_latched == NRF_LOGGER_LED_SEVERITY_ERROR);
    assert(led.output.reason_source == NRF_LOGGER_LED_SOURCE_BMI160_FAULT);
    assert(led.invalid_input_count == 2U);
}

static void test_null_arguments_fail_safe(void)
{
    nrf_logger_led_t led;
    nrf_logger_led_input_t in = ready_remote_input();
    nrf_logger_led_reset(&led);
    assert(nrf_logger_led_update(&led, 0, 0U) == NRF_LOGGER_LED_ERR_ARGUMENT);
    assert(led.output.red);
    assert(!led.output.green && !led.output.yellow);
    assert(led.output.severity_latched == NRF_LOGGER_LED_SEVERITY_ERROR);
    assert(led.output.reason_source == NRF_LOGGER_LED_SOURCE_INVALID_INPUT);
    assert(led.invalid_input_count == 1U);
    assert(nrf_logger_led_update(0, &in, 0U) == NRF_LOGGER_LED_ERR_ARGUMENT);
}

static void test_timer_wrap_duplicate_and_backwards_time(void)
{
    nrf_logger_led_t led;
    nrf_logger_led_input_t in = ready_remote_input();
    nrf_logger_led_reset(&led);
    in.source_levels[NRF_LOGGER_LED_SOURCE_ROLE_LOST] =
        NRF_LOGGER_LED_SEVERITY_WARNING;
    update_ok(&led, &in, UINT32_MAX - 1000U);
    update_ok(&led, &in, UINT32_MAX - 1U);
    assert(led.output.green);
    update_ok(&led, &in, 998U);
    assert(led.output.green);
    update_ok(&led, &in, 999U);
    assert(led.output.yellow);

    nrf_logger_led_reset(&led);
    in = ready_remote_input();
    in.source_levels[NRF_LOGGER_LED_SOURCE_ROLE_LOST] =
        NRF_LOGGER_LED_SEVERITY_WARNING;
    update_ok(&led, &in, 100U);
    update_ok(&led, &in, 100U);
    assert(led.output.green);
    assert(led.timestamp_anomaly_count == 1U);
    in.source_levels[NRF_LOGGER_LED_SOURCE_LOG_RING_LOSS] =
        NRF_LOGGER_LED_SEVERITY_ERROR;
    update_ok(&led, &in, 100U);
    assert(led.output.red);
    assert(led.timestamp_anomaly_count == 2U);

    nrf_logger_led_reset(&led);
    in = ready_remote_input();
    in.source_levels[NRF_LOGGER_LED_SOURCE_ROLE_LOST] =
        NRF_LOGGER_LED_SEVERITY_WARNING;
    update_ok(&led, &in, 2000U);
    update_ok(&led, &in, 1000U);
    assert(led.output.green);
    assert(led.timestamp_anomaly_count == 1U);
    update_ok(&led, &in, 3999U);
    assert(led.output.green);
    update_ok(&led, &in, 4000U);
    assert(led.output.yellow);
}

static void test_long_gaps_count_only_when_condition_is_still_reported(void)
{
    nrf_logger_led_t led;
    nrf_logger_led_input_t in = ready_remote_input();
    nrf_logger_led_reset(&led);
    in.source_levels[NRF_LOGGER_LED_SOURCE_ROLE_LOST] =
        NRF_LOGGER_LED_SEVERITY_WARNING;
    update_ok(&led, &in, 0U);
    update_ok(&led, &in, 5000U);
    assert(led.output.yellow);

    nrf_logger_led_reset(&led);
    in = ready_remote_input();
    in.source_levels[NRF_LOGGER_LED_SOURCE_ROLE_LOST] =
        NRF_LOGGER_LED_SEVERITY_WARNING;
    update_ok(&led, &in, 0U);
    in.source_levels[NRF_LOGGER_LED_SOURCE_ROLE_LOST] =
        NRF_LOGGER_LED_SEVERITY_NONE;
    update_ok(&led, &in, 5000U);
    assert(led.output.green);
    assert(led.output.severity_latched == NRF_LOGGER_LED_SEVERITY_NONE);
}

static void assert_same_output(
    const nrf_logger_led_output_t *left,
    const nrf_logger_led_output_t *right)
{
    assert(left->green == right->green);
    assert(left->yellow == right->yellow);
    assert(left->red == right->red);
    assert(left->severity_latched == right->severity_latched);
    assert(left->reason_source == right->reason_source);
}

static uint32_t xorshift32(uint32_t *state)
{
    *state ^= *state << 13U;
    *state ^= *state >> 17U;
    *state ^= *state << 5U;
    return *state;
}

static void test_four_node_isolation_and_randomized_invariants(void)
{
    nrf_logger_led_t nodes[4];
    uint32_t logical_ms[4] = {0U, 0U, 0U, 0U};
    uint32_t random_state = UINT32_C(0x6A09E667);

    for (size_t index = 0U; index < 4U; ++index) {
        nrf_logger_led_input_t in = ready_remote_input();
        nrf_logger_led_reset(&nodes[index]);
        update_ok(&nodes[index], &in, 0U);
    }

    nrf_logger_led_input_t fatal_input = ready_remote_input();
    fatal_input.source_levels[NRF_LOGGER_LED_SOURCE_INIT_BOOT_FATAL] =
        NRF_LOGGER_LED_SEVERITY_FATAL;
    update_ok(&nodes[0], &fatal_input, 1U);
    assert(nodes[0].output.red);
    for (size_t index = 1U; index < 4U; ++index) {
        assert(nodes[index].output.green);
        assert(nodes[index].output.severity_latched == NRF_LOGGER_LED_SEVERITY_NONE);
    }

    for (uint32_t step = 0U; step < 4096U; ++step) {
        uint32_t random_value = xorshift32(&random_state);
        size_t selected = (size_t)(random_value % 4U);
        nrf_logger_led_output_t before[4];
        nrf_logger_led_severity_t previous_severity[4];
        nrf_logger_led_input_t in = ready_remote_input();
        bool reset_now = step != 0U && step % 257U == 0U;

        for (size_t index = 0U; index < 4U; ++index) {
            before[index] = nodes[index].output;
            previous_severity[index] = nodes[index].output.severity_latched;
        }

        if (reset_now) {
            nrf_logger_led_reset(&nodes[selected]);
            logical_ms[selected] = 0U;
        }

        random_value = xorshift32(&random_state);
        logical_ms[selected] += random_value % 32U;
        in.bme280_ready = ((random_value >> 5U) & 1U) != 0U;
        switch ((random_value >> 8U) & 3U) {
        case 0U:
            in.node_state.role = NRF_NODE_ROLE_MAIN;
            break;
        case 1U:
            in.node_state.role = NRF_NODE_ROLE_UNASSIGNED;
            break;
        case 2U:
            in.node_state.role = (nrf_node_role_t)99;
            break;
        default:
            in.node_state.role = NRF_NODE_ROLE_REMOTE;
            break;
        }
        switch ((random_value >> 10U) & 3U) {
        case 0U:
            in.node_state.lifecycle = NRF_NODE_LIFECYCLE_BOOTING;
            break;
        case 1U:
            in.node_state.lifecycle = NRF_NODE_LIFECYCLE_UNAVAILABLE;
            break;
        case 2U:
            in.node_state.lifecycle = NRF_NODE_LIFECYCLE_READY;
            break;
        default:
            in.node_state.lifecycle = (nrf_node_lifecycle_t)99;
            break;
        }
        for (uint32_t source = 0U;
             source < (uint32_t)NRF_LOGGER_LED_SOURCE_COUNT;
             ++source) {
            in.source_levels[source] = (nrf_logger_led_severity_t)(
                xorshift32(&random_state) % 4U);
        }

        uint32_t now_ms = logical_ms[selected];
        if (!reset_now && step % 113U == 0U && now_ms != 0U) {
            --now_ms;
        }
        update_ok(&nodes[selected], &in, now_ms);

        for (size_t index = 0U; index < 4U; ++index) {
            assert_output_invariants(&nodes[index]);
            if (index == selected) {
                if (!reset_now) {
                    assert(nodes[index].output.severity_latched >=
                           previous_severity[index]);
                }
            } else {
                assert_same_output(&nodes[index].output, &before[index]);
            }
        }
    }
}

/*
 * The ESP32 learns why the logger LED turned yellow/red from a log event, so the
 * policy must hand out each latch change exactly once, with its source.
 */
static void test_latch_change_is_reported_once_with_its_source(void)
{
    nrf_logger_led_t led;
    nrf_logger_led_input_t in = ready_remote_input();
    nrf_logger_led_severity_t severity = NRF_LOGGER_LED_SEVERITY_NONE;
    nrf_logger_led_source_t source = NRF_LOGGER_LED_SOURCE_NONE;

    nrf_logger_led_reset(&led);
    update_ok(&led, &in, 0U);
    assert(!nrf_logger_led_take_latch_change(&led, &severity, &source));

    in.source_levels[NRF_LOGGER_LED_SOURCE_CS_RANGING] = NRF_LOGGER_LED_SEVERITY_ERROR;
    update_ok(&led, &in, 10U);
    assert(nrf_logger_led_take_latch_change(&led, &severity, &source));
    assert(severity == NRF_LOGGER_LED_SEVERITY_ERROR);
    assert(source == NRF_LOGGER_LED_SOURCE_CS_RANGING);
    assert(!nrf_logger_led_take_latch_change(&led, &severity, &source));
    update_ok(&led, &in, 20U);
    assert(!nrf_logger_led_take_latch_change(&led, &severity, &source));

    /* An equal severity from another source keeps the reason and is not a change. */
    in.source_levels[NRF_LOGGER_LED_SOURCE_BME280_FAULT] = NRF_LOGGER_LED_SEVERITY_ERROR;
    update_ok(&led, &in, 30U);
    assert(!nrf_logger_led_take_latch_change(&led, &severity, &source));

    /* A higher severity replaces the reason and is reported once more. */
    in.source_levels[NRF_LOGGER_LED_SOURCE_WATCHDOG_PARTICIPANT_STALE] = NRF_LOGGER_LED_SEVERITY_FATAL;
    update_ok(&led, &in, 40U);
    assert(nrf_logger_led_take_latch_change(&led, &severity, &source));
    assert(severity == NRF_LOGGER_LED_SEVERITY_FATAL);
    assert(source == NRF_LOGGER_LED_SOURCE_WATCHDOG_PARTICIPANT_STALE);
    assert(!nrf_logger_led_take_latch_change(&led, &severity, &source));

    /* A reboot/reset starts a new report cycle: the same latch is reported again. */
    nrf_logger_led_reset(&led);
    assert(!nrf_logger_led_take_latch_change(&led, &severity, &source));
    update_ok(&led, &in, 0U);
    assert(nrf_logger_led_take_latch_change(&led, &severity, &source));
    assert(severity == NRF_LOGGER_LED_SEVERITY_FATAL);
}

static void test_warning_latch_is_reported_after_confirmation_only(void)
{
    nrf_logger_led_t led;
    nrf_logger_led_input_t in = ready_remote_input();
    nrf_logger_led_severity_t severity;
    nrf_logger_led_source_t source;

    nrf_logger_led_reset(&led);
    in.source_levels[NRF_LOGGER_LED_SOURCE_TWIS_PROTOCOL] = NRF_LOGGER_LED_SEVERITY_WARNING;
    update_ok(&led, &in, 0U);
    update_ok(&led, &in, NRF_LOGGER_LED_WARNING_CONFIRM_MS - 1U);
    assert(!nrf_logger_led_take_latch_change(&led, &severity, &source));
    update_ok(&led, &in, NRF_LOGGER_LED_WARNING_CONFIRM_MS);
    assert(nrf_logger_led_take_latch_change(&led, &severity, &source));
    assert(severity == NRF_LOGGER_LED_SEVERITY_WARNING);
    assert(source == NRF_LOGGER_LED_SOURCE_TWIS_PROTOCOL);
}

static void test_invalid_input_latch_is_reported_and_null_arguments_are_safe(void)
{
    nrf_logger_led_t led;
    nrf_logger_led_severity_t severity = NRF_LOGGER_LED_SEVERITY_NONE;
    nrf_logger_led_source_t source = NRF_LOGGER_LED_SOURCE_NONE;

    nrf_logger_led_reset(&led);
    assert(nrf_logger_led_update(&led, 0, 0U) == NRF_LOGGER_LED_ERR_ARGUMENT);
    assert(nrf_logger_led_take_latch_change(&led, &severity, &source));
    assert(severity == NRF_LOGGER_LED_SEVERITY_ERROR);
    assert(source == NRF_LOGGER_LED_SOURCE_INVALID_INPUT);

    assert(!nrf_logger_led_take_latch_change(0, &severity, &source));
    assert(!nrf_logger_led_take_latch_change(&led, 0, &source));
    assert(!nrf_logger_led_take_latch_change(&led, &severity, 0));
}

int main(void)
{
    test_latch_change_is_reported_once_with_its_source();
    test_warning_latch_is_reported_after_confirmation_only();
    test_invalid_input_latch_is_reported_and_null_arguments_are_safe();
    test_persistent_runtime_state_mapping();
    test_pending_remote_sensor_preparation_does_not_latch();
    test_sensor_retry_does_not_clear_error_latch();
    test_bme280_is_required_after_remote_ready();
    test_red_suppresses_yellow_when_warning_and_error_are_latched();
    test_warning_requires_full_confirmation_and_flaps_restart();
    test_remote_ready_truth_table_for_each_source_and_level();
    test_errors_and_fatals_latch_immediately();
    test_reason_changes_only_when_latched_severity_rises();
    test_latch_persists_until_reset();
    test_main_uses_common_health_without_remote_sensors();
    test_unassigned_and_unknown_roles_fail_off();
    test_non_ready_remote_never_shows_green();
    test_invalid_source_levels_are_errors_and_counted();
    test_null_arguments_fail_safe();
    test_timer_wrap_duplicate_and_backwards_time();
    test_long_gaps_count_only_when_condition_is_still_reported();
    test_four_node_isolation_and_randomized_invariants();
    return 0;
}
