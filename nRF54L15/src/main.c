/*
 * XIAO nRF54L15 production bring-up.
 *
 * All boards boot with an UNASSIGNED node role and expose the same upstream
 * bootstrap protocol at the DISCOVERY I2C address. Runtime assignment decides
 * MAIN versus REMOTE; there are no separate production firmware variants.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <nrfx_power.h>

#include "nrf_bootstrap_runtime_zephyr.h"
#include "nrf_boot_diagnostics.h"
#include "nrf_boot_failure_reason.h"
#include "nrf_blue_led.h"
#include "nrf_cs_pair_runtime_zephyr.h"
#include "nrf_fault_record_zephyr.h"
#include "nrf_logger_led_zephyr.h"
#include "nrf_node_state.h"
#include "nrf_log_events.h"
#include "nrf_log_runtime_zephyr.h"
#include "nrf_sensor_upstream_runtime_zephyr.h"
#include "nrf_watchdog_zephyr.h"

#include <zephyr/sys/printk.h>

#define MAIN_TICK_PERIOD_MS 1000U
#define DIAGNOSTIC_POLL_PERIOD_MS 50U
#define LED0_NODE DT_ALIAS(led0)

static const struct gpio_dt_spec led =
    GPIO_DT_SPEC_GET(LED0_NODE, gpios);

static const char *logger_pin_state_name(nrf_logger_led_pin_state_t state)
{
    switch (state) {
    case NRF_LOGGER_LED_PIN_READY:
        return "ready";
    case NRF_LOGGER_LED_PIN_UNAVAILABLE:
        return "unavailable";
    case NRF_LOGGER_LED_PIN_FAILED:
        return "failed";
    default:
        return "invalid";
    }
}

#if defined(CONFIG_APP_LED_BOOT_LAMP_TEST) && CONFIG_APP_LED_BOOT_LAMP_TEST
static int set_boot_lamp(void *context, nrf_boot_lamp_t lamp, bool on)
{
    (void)context;
    switch (lamp) {
    case NRF_BOOT_LAMP_BLUE: {
        const int result = nrf_blue_led_zephyr_set_test_pin(on);
        return result == -ENODEV ? NRF_BOOT_LAMP_UNAVAILABLE : result;
    }
    case NRF_BOOT_LAMP_GREEN:
        {
            const int result = nrf_logger_led_zephyr_set_test_pin(
                NRF_LOGGER_LED_COLOR_GREEN, on);
            return result == NRF_LOGGER_LED_ZEPHYR_UNAVAILABLE
                ? NRF_BOOT_LAMP_UNAVAILABLE
                : result;
        }
    case NRF_BOOT_LAMP_YELLOW:
        {
            const int result = nrf_logger_led_zephyr_set_test_pin(
                NRF_LOGGER_LED_COLOR_YELLOW, on);
            return result == NRF_LOGGER_LED_ZEPHYR_UNAVAILABLE
                ? NRF_BOOT_LAMP_UNAVAILABLE
                : result;
        }
    case NRF_BOOT_LAMP_RED:
        {
            const int result = nrf_logger_led_zephyr_set_test_pin(
                NRF_LOGGER_LED_COLOR_RED, on);
            return result == NRF_LOGGER_LED_ZEPHYR_UNAVAILABLE
                ? NRF_BOOT_LAMP_UNAVAILABLE
                : result;
        }
    default:
        return -EINVAL;
    }
}

static void sleep_boot_lamp(void *context, uint32_t duration_ms)
{
    (void)context;
    k_msleep((int32_t)duration_ms);
}
#endif

static void run_boot_lamp_test(void)
{
#if defined(CONFIG_APP_LED_BOOT_LAMP_TEST) && CONFIG_APP_LED_BOOT_LAMP_TEST
    nrf_boot_lamp_result_t results[NRF_BOOT_LAMP_COUNT];
    nrf_boot_lamp_summary_t summary;

    if (nrf_boot_lamp_test_run(
            set_boot_lamp,
            sleep_boot_lamp,
            NULL,
            results,
            &summary) != 0) {
        printk("lamp_test: failed to start\n");
        return;
    }

    for (size_t index = 0U; index < (size_t)NRF_BOOT_LAMP_COUNT; ++index) {
        if (!results[index].available) {
            printk("lamp_test: %s unavailable\n",
                   nrf_boot_lamp_name((nrf_boot_lamp_t)index));
        } else {
            printk("lamp_test: %s on/off ret=%d/%d\n",
                   nrf_boot_lamp_name((nrf_boot_lamp_t)index),
                   results[index].on_result,
                   results[index].off_result);
        }
    }
    printk("lamp_test: summary tested=%u unavailable=%u errors=%u; "
           "if a lamp stayed dark: check wiring/polarity (MAX-43)\n",
           (unsigned int)summary.tested_count,
           (unsigned int)summary.unavailable_count,
           (unsigned int)summary.error_count);
#endif
}

#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG && \
    defined(CONFIG_APP_NRF_STATUS_PRINT_PERIOD_MS) && \
    CONFIG_APP_NRF_STATUS_PRINT_PERIOD_MS > 0
static void print_periodic_status(const nrf_node_state_t *node_state)
{
    nrf_boot_status_t status = {0};
    nrf_bootstrap_twis_diagnostics_t twis = {0};
    nrf_sensor_upstream_runtime_zephyr_diagnostics_t sensor = {0};
    nrf_log_runtime_zephyr_led_counters_t log_counters = {0};
    char line[NRF_BOOT_STATUS_LINE_CAPACITY];
    int result;

    status.uptime_ms = (uint64_t)k_uptime_get();
    status.lifecycle = node_state->lifecycle;
    status.role = node_state->role;
    nrf_bootstrap_runtime_zephyr_get_twis_diagnostics(&twis);
    if (twis.role_assignment_sequence != 0U) {
        status.role = (nrf_node_role_t)twis.role_assignment_role;
    }
    status.write_received_count = twis.write_received_count;
    status.write_received_bytes = twis.write_received_bytes;
    status.read_requested_count = twis.read_requested_count;
    status.callback_error_count = twis.callback_error_count;
    status.last_request_type_valid = twis.last_request_type_valid;
    status.last_request_type = twis.last_request_type;
    status.last_request_type_count = twis.last_request_type_count;
    status.diagnostic_ring_dropped_total = twis.diagnostic_ring_dropped_total;

    if (nrf_sensor_upstream_runtime_zephyr_get_diagnostics(&sensor)) {
        status.imu_ready = sensor.imu_ready;
        status.imu_init_result = sensor.imu_init_result;
        status.env_ready = sensor.env_ready;
        status.env_init_result = sensor.env_init_result;
        status.logger_led_output = sensor.logger_led_output;
        status.logger_policy_invalid_count = sensor.logger_policy_invalid_count;
        status.logger_policy_timestamp_anomaly_count =
            sensor.logger_policy_timestamp_anomaly_count;
    }

    status.blue_led_state = nrf_blue_led_zephyr_state();
    status.blue_led_failure_count = nrf_blue_led_zephyr_failure_count();
    status.logger_gpio_failure_count = nrf_logger_led_zephyr_failure_count();
    if (nrf_log_runtime_zephyr_get_led_counters(&log_counters) == NRF_LOG_ERROR_OK) {
        status.log_overwritten_total = log_counters.overwritten_total;
        status.log_lost_internal_total = log_counters.lost_internal_total;
    }

    result = nrf_boot_status_format(&status, line, sizeof(line));
    if (result >= 0) {
        printk("%s\n", line);
    } else {
        printk("status: format error=%d\n", result);
    }
}
#endif

static void drain_twis_transaction_records(void)
{
#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
    static nrf_boot_twis_log_state_t log_state;
    nrf_bootstrap_twis_diagnostics_t diagnostics = {0};
    nrf_bootstrap_runtime_zephyr_get_twis_diagnostics(&diagnostics);
#endif
    nrf_boot_twis_record_t record;
    char line[NRF_BOOT_TRANSACTION_LINE_CAPACITY];

    while (nrf_bootstrap_runtime_zephyr_drain_twis_record(&record)) {
#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
        const nrf_boot_twis_log_action_t action = nrf_boot_twis_log_decide(
            &log_state,
            &record,
            (uint64_t)k_uptime_get());

        if (record.index == 1U || action == NRF_BOOT_TWIS_LOG_DETAIL ||
            action == NRF_BOOT_TWIS_LOG_ERROR) {
            if (nrf_boot_twis_record_format(&record, line, sizeof(line)) >= 0) {
                printk("%s\n", line);
            }
        } else if (action == NRF_BOOT_TWIS_LOG_SUMMARY) {
            if (nrf_boot_twis_summary_format(
                    &log_state,
                    diagnostics.diagnostic_ring_dropped_total,
                    line,
                    sizeof(line)) >= 0) {
                printk("%s\n", line);
            }
        }
#else
        if (record.index == 1U &&
            nrf_boot_twis_record_format(&record, line, sizeof(line)) >= 0) {
            printk("%s\n", line);
        }
#endif
    }
}

#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
static void print_state_transitions(const nrf_node_state_t *node_state)
{
    static nrf_boot_change_detector_t detector;
    nrf_bootstrap_twis_diagnostics_t twis = {0};
    nrf_sensor_upstream_runtime_zephyr_diagnostics_t sensor = {0};
    nrf_boot_change_snapshot_t snapshot = {0};
    const nrf_boot_change_snapshot_t previous = detector.last;
    const bool had_previous = detector.initialized;
    const bool have_sensor_diagnostics =
        nrf_sensor_upstream_runtime_zephyr_get_diagnostics(&sensor);
    const uint32_t now_ms = k_uptime_get_32();
    uint32_t changes;
    bool activation_completed;

    nrf_bootstrap_runtime_zephyr_get_twis_diagnostics(&twis);
    snapshot.lifecycle = (uint8_t)node_state->lifecycle;
    snapshot.role = twis.role_assignment_sequence != 0U
        ? twis.role_assignment_role
        : (uint8_t)node_state->role;
    snapshot.node_id = twis.role_assignment_node_id;
    snapshot.role_assignment_sequence = twis.role_assignment_sequence;
    snapshot.sensor_diagnostics_valid = have_sensor_diagnostics;
    if (have_sensor_diagnostics) {
        snapshot.sensor_activation_sequence = sensor.activation_generation;
        snapshot.sensor_activation_complete_sequence =
            sensor.activation_complete_generation;
        snapshot.imu_ready = sensor.imu_ready;
        snapshot.imu_init_result = sensor.imu_init_result;
        snapshot.env_ready = sensor.env_ready;
        snapshot.env_init_result = sensor.env_init_result;
        snapshot.logger_green = sensor.logger_led_output.green;
        snapshot.logger_yellow = sensor.logger_led_output.yellow;
        snapshot.logger_red = sensor.logger_led_output.red;
        snapshot.logger_severity =
            (uint8_t)sensor.logger_led_output.severity_latched;
        snapshot.logger_reason_source =
            (uint8_t)sensor.logger_led_output.reason_source;
        snapshot.orientation_state = sensor.orientation_state;
        snapshot.verticality_state = sensor.verticality_state;
    }
    snapshot.blue_led_state = nrf_blue_led_zephyr_state();

    changes = nrf_boot_change_detector_update(&detector, &snapshot);
    activation_completed =
        snapshot.sensor_activation_complete_sequence !=
        (had_previous ? previous.sensor_activation_complete_sequence : 0U);

    if ((changes & NRF_BOOT_CHANGE_LIFECYCLE) != 0U) {
        printk("state: lifecycle=%s up=%" PRIu32 "ms\n",
               nrf_boot_lifecycle_name(node_state->lifecycle),
               now_ms);
    }
    if ((changes & NRF_BOOT_CHANGE_ROLE_ASSIGNMENT) != 0U &&
        snapshot.role_assignment_sequence != 0U &&
        (!had_previous || snapshot.role_assignment_sequence !=
            previous.role_assignment_sequence)) {
        printk("state: role_assignment role=%s node_id=%" PRIu32
               " up=%" PRIu32 "ms\n",
               nrf_boot_role_name((nrf_node_role_t)snapshot.role),
               snapshot.node_id,
               now_ms);
    }
    if ((changes & NRF_BOOT_CHANGE_SENSOR_ACTIVATION) != 0U &&
        snapshot.sensor_activation_sequence != 0U &&
        (!had_previous || snapshot.sensor_activation_sequence !=
            previous.sensor_activation_sequence)) {
        printk("sensor: activation_start generation=%" PRIu32
               " role=%s node_id=%" PRIu32 " up=%" PRIu32 "ms\n",
               snapshot.sensor_activation_sequence,
               nrf_boot_role_name((nrf_node_role_t)snapshot.role),
               snapshot.node_id,
               now_ms);
    }
    if (activation_completed && have_sensor_diagnostics) {
        printk("sensor: activation_result generation=%" PRIu32
               " bmi160_ready=%u bmi160_init=%" PRId32
               " bme280_ready=%u bme280_init=%" PRId32
               " up=%" PRIu32 "ms\n",
               snapshot.sensor_activation_complete_sequence,
               snapshot.imu_ready ? 1U : 0U,
               snapshot.imu_init_result,
               snapshot.env_ready ? 1U : 0U,
               snapshot.env_init_result,
               now_ms);
    } else if ((changes & NRF_BOOT_CHANGE_SENSOR_RESULT) != 0U &&
               have_sensor_diagnostics) {
        printk("sensor: state bmi160_ready=%u bmi160_init=%" PRId32
               " bme280_ready=%u bme280_init=%" PRId32
               " up=%" PRIu32 "ms\n",
               snapshot.imu_ready ? 1U : 0U,
               snapshot.imu_init_result,
               snapshot.env_ready ? 1U : 0U,
               snapshot.env_init_result,
               now_ms);
    }
    if ((changes & NRF_BOOT_CHANGE_BLUE_LED) != 0U) {
        printk("state: blue_led=%s up=%" PRIu32 "ms\n",
               nrf_boot_blue_led_name(snapshot.blue_led_state),
               now_ms);
    }
    if ((changes & NRF_BOOT_CHANGE_LOGGER_OUTPUT) != 0U &&
        have_sensor_diagnostics) {
        printk("state: logger_led g/y/r=%u/%u/%u up=%" PRIu32 "ms\n",
               snapshot.logger_green ? 1U : 0U,
               snapshot.logger_yellow ? 1U : 0U,
               snapshot.logger_red ? 1U : 0U,
               now_ms);
    }
    if ((changes & NRF_BOOT_CHANGE_LOGGER_LATCH) != 0U &&
        have_sensor_diagnostics) {
        printk("state: logger_latch source=%s severity=%s up=%" PRIu32 "ms\n",
               nrf_boot_logger_source_name(snapshot.logger_reason_source),
               nrf_boot_severity_name(
                   (nrf_logger_led_severity_t)snapshot.logger_severity),
               now_ms);
    }
    if ((changes & NRF_BOOT_CHANGE_ORIENTATION) != 0U &&
        have_sensor_diagnostics) {
        printk("state: orientation=%s verticality=%s up=%" PRIu32 "ms\n",
               nrf_boot_orientation_name(snapshot.orientation_state),
               nrf_boot_verticality_name(snapshot.verticality_state),
               now_ms);
    }
}
#endif

#if defined(CONFIG_APP_NRF_STAGE_SIGNALS) && CONFIG_APP_NRF_STAGE_SIGNALS
static void note_stage_signal_upstream(void)
{
    nrf_bootstrap_twis_diagnostics_t diagnostics = {0};

    nrf_bootstrap_runtime_zephyr_get_twis_diagnostics(&diagnostics);
    nrf_sensor_upstream_runtime_zephyr_note_upstream(
        diagnostics.transaction_count,
        diagnostics.role_assignment_sequence,
        diagnostics.role_assignment_role);
}
#endif

int main(void)
{
    int ret;
    bool led_is_on = true;
    nrf_node_state_t node_state;
#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG && \
    defined(CONFIG_APP_NRF_STATUS_PRINT_PERIOD_MS) && \
    CONFIG_APP_NRF_STATUS_PRINT_PERIOD_MS > 0
    uint64_t last_status_ms = 0U;
#endif
    uint64_t next_main_tick_ms = 0U;

    ret = nrf_log_runtime_zephyr_init();
    printk("boot: nrf_log_runtime_zephyr_init ret=%d\n", ret);
    if (ret != 0) {
        return ret;
    }
    nrf_fault_record_zephyr_report_pending();

    {
        const nrf_logger_led_zephyr_result_t logger_result =
            nrf_logger_led_zephyr_init();
        printk("boot: nrf_logger_led_zephyr_init ret=%d pins=g:%s,y:%s,r:%s\n",
               (int)logger_result,
               logger_pin_state_name(nrf_logger_led_zephyr_pin_state(
                   NRF_LOGGER_LED_COLOR_GREEN)),
               logger_pin_state_name(nrf_logger_led_zephyr_pin_state(
                   NRF_LOGGER_LED_COLOR_YELLOW)),
               logger_pin_state_name(nrf_logger_led_zephyr_pin_state(
                   NRF_LOGGER_LED_COLOR_RED)));
    }

    ret = nrf_blue_led_zephyr_init();
    printk("boot: nrf_blue_led_zephyr_init ret=%d state=%s\n",
           ret,
           nrf_boot_blue_led_name(nrf_blue_led_zephyr_state()));

#if defined(CONFIG_NRFX_POWER)
    nrfx_power_constlat_mode_request();
    printk("boot: constant_latency_request issued\n");
#endif

    nrf_node_state_init(&node_state);
    printk("boot: node_state_init ret=0\n");
#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
    print_state_transitions(&node_state);
#endif

    if (!gpio_is_ready_dt(&led)) {
        printk("boot: onboard_led0_init ret=-ENODEV\n");
        return -1;
    }

    ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
    printk("boot: onboard_led0_init ret=%d\n", ret);
    if (ret < 0) {
        return ret;
    }

    run_boot_lamp_test();

    {
        const nrf_node_state_result_t state_result =
            nrf_node_state_mark_ready(&node_state);
        printk("boot: node_state_mark_ready ret=%d\n", (int)state_result);
        if (state_result != NRF_NODE_STATE_OK) {
            return -1;
        }
    }

    ret = nrf_sensor_upstream_runtime_zephyr_start();
    printk("boot: nrf_sensor_upstream_runtime_zephyr_start ret=%d\n", ret);
    if (ret != 0) {
        return ret;
    }

    ret = nrf_cs_pair_runtime_zephyr_start();
    printk("boot: nrf_cs_pair_runtime_zephyr_start ret=%d\n", ret);
    if (ret != 0) {
        nrf_log_runtime_zephyr_append_event(
            NRF_LOG_EVENT_BOOT_FAILED,
            (uint32_t)nrf_boot_failure_reason_from_errno(ret),
            (uint32_t)ret);
    }

    ret = nrf_bootstrap_runtime_zephyr_start(&node_state);
    printk("boot: nrf_bootstrap_runtime_zephyr_start ret=%d\n", ret);
    if (ret != 0) {
        return ret;
    }

    /* MAX-244: armed last, once every monitored runtime exists. A failure
     * leaves the node running unprotected instead of refusing to boot. */
    ret = nrf_watchdog_zephyr_start();
    printk("boot: nrf_watchdog_zephyr_start ret=%d timeout_ms=%u\n",
           ret,
           (unsigned int)NRF_WATCHDOG_TIMEOUT_MS);
    if (ret < 0) {
        nrf_logger_led_zephyr_latch_init_fatal();
        printk("nrf_watchdog: NOT armed (%d); a hung node will not self-reset\n", ret);
    }
    printk("boot: complete build=%s %s\n", __DATE__, __TIME__);

    while (1) {
        const uint64_t now_ms = (uint64_t)k_uptime_get();

        if (now_ms >= next_main_tick_ms) {
#if defined(CONFIG_APP_LOG_TEST_EVENTS) && CONFIG_APP_LOG_TEST_EVENTS
            static uint32_t test_event_index;
            for (uint32_t burst_index = 0U; burst_index < 32U; ++burst_index) {
                nrf_log_runtime_zephyr_append_event(
                    NRF_LOG_EVENT_LOG_TEST_EVENT,
                    ++test_event_index,
                    (uint32_t)now_ms);
            }
#endif
            nrf_watchdog_zephyr_set_participant_active(
                NRF_WATCHDOG_PARTICIPANT_BT_CS_PROGRESS,
                nrf_cs_pair_runtime_zephyr_is_progress_required());
            nrf_watchdog_zephyr_service();

            ret = gpio_pin_set_dt(&led, (int)led_is_on);
            if (ret < 0) {
                printk("runtime: onboard_led0_set ret=%d\n", ret);
                return ret;
            }

            led_is_on = !led_is_on;
            next_main_tick_ms = now_ms + MAIN_TICK_PERIOD_MS;
        }

        drain_twis_transaction_records();
#if defined(CONFIG_APP_NRF_STAGE_SIGNALS) && CONFIG_APP_NRF_STAGE_SIGNALS
        note_stage_signal_upstream();
#endif
#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
        print_state_transitions(&node_state);
#endif
#if defined(CONFIG_APP_NRF_STATUS_PRINT_PERIOD_MS) && \
    defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG && \
    CONFIG_APP_NRF_STATUS_PRINT_PERIOD_MS > 0
        if (now_ms - last_status_ms >=
            (uint64_t)CONFIG_APP_NRF_STATUS_PRINT_PERIOD_MS) {
            print_periodic_status(&node_state);
            last_status_ms = now_ms;
        }
#endif
        k_msleep((int32_t)DIAGNOSTIC_POLL_PERIOD_MS);
    }

    return 0;
}
