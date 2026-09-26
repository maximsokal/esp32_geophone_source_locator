#include "ina3221_monitor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "app_task_config.h"
#include "device_status.h"
#include "ina3221_driver.h"
#include "ina3221_limits.h"
#include "ina3221_low_battery.h"
#include "ina3221_power_ux.h"
#include "ina3221_soc.h"
#include "app_events.h"
#include "ina3221_policy.h"
#include "main_barometer.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* The Kconfig limits must reach this file on the target (FreeRTOS.h pulls
 * in sdkconfig.h); silently falling back to defaults would hide a typo. */
#if defined(ESP_PLATFORM) && !defined(CONFIG_INA3221_AD7606_CRITICAL_MA)
#error "INA3221 over-current Kconfig limits are not visible"
#endif


#define INA3221_MONITOR_PERIOD_MS       1000u
#define INA3221_MONITOR_FAST_PERIOD_MS  20u
#define INA3221_MONITOR_LOG_INTERVAL    5u
#define INA3221_RETRY_LOG_INTERVAL      30u


static const char *TAG =
    "INA3221";


static TaskHandle_t s_task =
    NULL;

static portMUX_TYPE s_state_lock =
    portMUX_INITIALIZER_UNLOCKED;

static bool s_starting =
    false;

static bool s_stop_requested =
    false;

/* MAX-242: last published per-rail readings, guarded by s_state_lock. */
static ina3221_monitor_rail_t s_rails[INA3221_POLICY_CHANNEL_COUNT];

static void ina3221_monitor_store_rail(size_t index, bool valid, float voltage_v, float current_a)
{
    const int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_state_lock);
    s_rails[index].available = true;
    s_rails[index].valid = valid;
    s_rails[index].bus_voltage_v = voltage_v;
    s_rails[index].current_a = current_a;
    s_rails[index].sampled_at_us = now_us;
    portEXIT_CRITICAL(&s_state_lock);
}

static int64_t s_fast_sampling_deadline_us =
    0;

/* MAX-245: limits are fixed per run; the tracker is owned by the worker.
 * Clear requests from other tasks are a channel bit mask under
 * s_state_lock. */
static ina3221_limits_t s_limits;
static ina3221_overcurrent_tracker_t s_overcurrent;
static uint8_t s_overcurrent_clear_mask = 0U;

/* MAX-180/183: owned by the worker. */
static ina3221_power_ux_config_t s_ux_config;
static ina3221_power_ux_t s_ux;
static ina3221_low_battery_t s_low_battery;
static ina3221_monitor_critical_battery_handler_t s_critical_battery_handler;
/* MAX-181 */
static ina3221_soc_config_t s_soc_config;
static ina3221_soc_t s_soc;
/* MAX-186: last logged evidence (worker only). */
static ina3221_power_evidence_t s_evidence;


static void ina3221_monitor_log_evidence_changes(
    const ina3221_power_decision_t *decision
)
{
    const ina3221_power_evidence_t current = {
        .observed = true,
        .battery_present = decision->battery_present,
        .usb_present = decision->usb_present,
        .charging = decision->charging,
        .soc_quality = (int)s_soc.quality,
    };
    const uint32_t changes = ina3221_power_evidence_changes(&s_evidence, &current);
    if ((changes & INA3221_EVIDENCE_BATTERY_CHANGED) != 0U) {
        ESP_LOGI(TAG, "POWER_EVIDENCE battery %s (%.3f V)",
                 current.battery_present ? "present" : "absent",
                 (double)decision->battery_voltage_v);
    }
    if ((changes & INA3221_EVIDENCE_USB_CHANGED) != 0U) {
        ESP_LOGI(TAG, "POWER_EVIDENCE USB %s", current.usb_present ? "present" : "absent");
    }
    if ((changes & INA3221_EVIDENCE_CHARGING_CHANGED) != 0U) {
        ESP_LOGI(TAG, "POWER_EVIDENCE charging %s (%+.3f A)",
                 current.charging ? "started" : "stopped",
                 (double)decision->battery_current_a);
    }
    if ((changes & INA3221_EVIDENCE_SOC_CHANGED) != 0U) {
        ESP_LOGI(TAG, "POWER_EVIDENCE SoC source %s -> %s (%.0f%%)",
                 ina3221_soc_quality_name((ina3221_soc_quality_t)s_evidence.soc_quality),
                 ina3221_soc_quality_name(s_soc.quality),
                 (double)decision->battery_percent_estimate);
    }
    s_evidence = current;
}

_Static_assert(INA3221_SOC_FULL_ANCHORED == 3 && DEVICE_BATTERY_SOC_FULL_ANCHORED == 3 &&
               INA3221_SOC_VOLTAGE_ONLY == 1 && DEVICE_BATTERY_SOC_VOLTAGE_ONLY == 1,
               "device_battery_soc_quality_t must mirror ina3221_soc_quality_t");


/* MAX-181: replace the voltage-only percentage with the hybrid estimate and
 * record where it came from. */
static void ina3221_monitor_apply_soc(
    ina3221_power_decision_t *decision
)
{
    const ina3221_soc_input_t input = {
        .valid = decision->battery_present,
        .voltage_v = decision->battery_voltage_v,
        .current_a = decision->battery_current_a,
        .usb_present = decision->usb_present,
        .charging = decision->charging,
    };
    ina3221_soc_update(&s_soc, &s_soc_config, &input, esp_timer_get_time() / 1000);
    if (decision->battery_present && s_soc.quality != INA3221_SOC_INVALID) {
        decision->battery_percent_estimate = ina3221_soc_percent(&s_soc, &s_soc_config);
        decision->battery_percent_valid = true;
    }
    (void)device_status_set_battery_soc_quality(
        (device_battery_soc_quality_t)s_soc.quality
    );
}

/* device_status mirrors these enums value by value (it must not depend on
 * this component); pin both sides to the same numbers. */
_Static_assert(INA3221_POWER_UX_USB_BATTERY_READY == 3 && INA3221_POWER_UX_BATTERY_CRITICAL == 6,
               "ina3221_power_ux_state_t numbering changed");
_Static_assert(DEVICE_POWER_STATE_USB_BATTERY_READY == 3 && DEVICE_POWER_STATE_BATTERY_CRITICAL == 6,
               "device_power_state_t must mirror ina3221_power_ux_state_t");
_Static_assert(INA3221_LOW_BATTERY_GRACE == 2 && INA3221_LOW_BATTERY_SHUTDOWN == 3,
               "ina3221_low_battery_phase_t numbering changed");
_Static_assert(DEVICE_LOW_BATTERY_GRACE == 2 && DEVICE_LOW_BATTERY_SHUTDOWN == 3,
               "device_low_battery_phase_t must mirror ina3221_low_battery_phase_t");


static void ina3221_monitor_load_power_ux(void)
{
    ina3221_power_ux_default_config(&s_ux_config);
    uint32_t grace_ms = 10000U;
#if defined(CONFIG_POWER_BATTERY_LOW_ENTER_MV)
    s_ux_config.low_enter_v = (float)CONFIG_POWER_BATTERY_LOW_ENTER_MV / 1000.0f;
    s_ux_config.low_exit_v = (float)CONFIG_POWER_BATTERY_LOW_EXIT_MV / 1000.0f;
    s_ux_config.critical_enter_v = (float)CONFIG_POWER_BATTERY_CRITICAL_ENTER_MV / 1000.0f;
    s_ux_config.critical_exit_v = (float)CONFIG_POWER_BATTERY_CRITICAL_EXIT_MV / 1000.0f;
    s_ux_config.full_v = (float)CONFIG_POWER_BATTERY_FULL_MV / 1000.0f;
    grace_ms = (uint32_t)CONFIG_POWER_LOW_BATTERY_GRACE_S * 1000U;
#endif
    if (!ina3221_power_ux_config_valid(&s_ux_config)) {
        ESP_LOGE(TAG, "Configured battery thresholds are inconsistent "
                      "(critical < low, enter < exit); using the built-in defaults");
        ina3221_power_ux_default_config(&s_ux_config);
    }
    ina3221_power_ux_reset(&s_ux);
    ina3221_low_battery_init(&s_low_battery, grace_ms);

    ina3221_soc_default_config(&s_soc_config);
    s_soc_config.full_v = s_ux_config.full_v;
#if defined(CONFIG_POWER_BATTERY_CAPACITY_MAH)
    s_soc_config.capacity_mah = (float)CONFIG_POWER_BATTERY_CAPACITY_MAH;
#endif
    ina3221_soc_reset(&s_soc);
    memset(&s_evidence, 0, sizeof(s_evidence));
}


static void ina3221_monitor_apply_power_state(
    ina3221_power_state_t source_state,
    const ina3221_power_decision_t *decision,
    bool telemetry_valid
)
{
    const ina3221_power_ux_input_t input = {
        .source = source_state,
        .telemetry_valid = telemetry_valid,
        .battery_present = decision != NULL && decision->battery_present,
        .battery_voltage_v = decision != NULL ? decision->battery_voltage_v : 0.0f,
    };
    const ina3221_power_ux_state_t previous = s_ux.state;
    const ina3221_power_ux_state_t state =
        ina3221_power_ux_update(&s_ux, &s_ux_config, &input);
    if (state != previous) {
        ESP_LOGI(
            TAG,
            "Power state %s -> %s (battery %.3f V)",
            ina3221_power_ux_state_name(previous),
            ina3221_power_ux_state_name(state),
            (double)input.battery_voltage_v
        );
    }

    const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    const ina3221_low_battery_action_t action =
        ina3221_low_battery_update(&s_low_battery, state, now_ms);
    const uint32_t shutdown_in_s =
        ina3221_low_battery_remaining_s(&s_low_battery, now_ms);

    switch (action) {
        case INA3221_LOW_BATTERY_ACTION_WARN:
            ESP_LOGW(TAG, "LOW BATTERY: %.3f V", (double)input.battery_voltage_v);
            (void)app_events_post_best_effort(APP_EVENT_POWER_BATTERY_ALERT, APP_POWER_ALERT_LOW);
            break;
        case INA3221_LOW_BATTERY_ACTION_START_GRACE:
            ESP_LOGE(
                TAG,
                "CRITICAL BATTERY %.3f V: connect USB within %u s or the device shuts down",
                (double)input.battery_voltage_v,
                (unsigned)shutdown_in_s
            );
            (void)app_events_post_best_effort(APP_EVENT_POWER_BATTERY_ALERT, APP_POWER_ALERT_CRITICAL_GRACE);
            break;
        case INA3221_LOW_BATTERY_ACTION_CANCELLED:
            ESP_LOGW(TAG, "USB power restored: low-battery shutdown cancelled");
            (void)app_events_post_best_effort(APP_EVENT_POWER_BATTERY_ALERT, APP_POWER_ALERT_CANCELLED);
            break;
        case INA3221_LOW_BATTERY_ACTION_REQUEST_SHUTDOWN:
            ESP_LOGE(TAG, "CRITICAL BATTERY: starting safe shutdown (recorder, SD, power off)");
            (void)app_events_post_best_effort(APP_EVENT_POWER_BATTERY_ALERT, APP_POWER_ALERT_SHUTDOWN);
            if (s_critical_battery_handler != NULL) {
                s_critical_battery_handler();
            } else {
                ESP_LOGE(TAG, "No critical-battery handler registered; cannot shut down");
            }
            break;
        case INA3221_LOW_BATTERY_ACTION_NONE:
        default:
            break;
    }

    (void)device_status_set_power_state(
        (device_power_state_t)state,
        state == INA3221_POWER_UX_USB_BATTERY_READY &&
            input.battery_voltage_v >= s_ux_config.full_v,
        (device_low_battery_phase_t)s_low_battery.phase,
        shutdown_in_s
    );
}


static void ina3221_monitor_load_limits(void)
{
    ina3221_limits_default(&s_limits);
#if defined(CONFIG_INA3221_BATTERY_WARNING_MA)
    s_limits.channels[INA3221_POLICY_BATTERY_INDEX].warning_a =
        (float)CONFIG_INA3221_BATTERY_WARNING_MA / 1000.0f;
    s_limits.channels[INA3221_POLICY_BATTERY_INDEX].critical_a =
        (float)CONFIG_INA3221_BATTERY_CRITICAL_MA / 1000.0f;
    s_limits.channels[INA3221_POLICY_ESP32_INDEX].warning_a =
        (float)CONFIG_INA3221_ESP32_WARNING_MA / 1000.0f;
    s_limits.channels[INA3221_POLICY_ESP32_INDEX].critical_a =
        (float)CONFIG_INA3221_ESP32_CRITICAL_MA / 1000.0f;
    s_limits.channels[INA3221_POLICY_AD7606_INDEX].warning_a =
        (float)CONFIG_INA3221_AD7606_WARNING_MA / 1000.0f;
    s_limits.channels[INA3221_POLICY_AD7606_INDEX].critical_a =
        (float)CONFIG_INA3221_AD7606_CRITICAL_MA / 1000.0f;
#endif
    if (!ina3221_limits_valid(&s_limits, 0.1f)) {
        ESP_LOGE(TAG, "Configured over-current limits are invalid (warning must be below critical, "
                      "critical <= 1.63 A); using the built-in defaults");
        ina3221_limits_default(&s_limits);
    }
}


static void ina3221_monitor_apply_overcurrent(
    const ina3221_channel_sample_t channels[INA3221_POLICY_CHANNEL_COUNT]
)
{
    portENTER_CRITICAL(&s_state_lock);
    const uint8_t clear_mask = s_overcurrent_clear_mask;
    s_overcurrent_clear_mask = 0U;
    portEXIT_CRITICAL(&s_state_lock);

    for (size_t i = 0; i < INA3221_POLICY_CHANNEL_COUNT; ++i) {
        if ((clear_mask & (uint8_t)(1U << i)) != 0U) {
            ina3221_overcurrent_clear_latch(&s_overcurrent, i);
        }
    }

    if (ina3221_overcurrent_update(&s_overcurrent, &s_limits, channels)) {
        for (size_t i = 0; i < INA3221_POLICY_CHANNEL_COUNT; ++i) {
            const ina3221_overcurrent_channel_t *oc = &s_overcurrent.channels[i];
            if (oc->latched && oc->level == INA3221_OVERCURRENT_CRITICAL) {
                ESP_LOGE(
                    TAG,
                    "OVER-CURRENT %s: %.3f A >= %.3f A (latched)",
                    ina3221_policy_channel_name(i),
                    (double)oc->latched_current_a,
                    (double)s_limits.channels[i].critical_a
                );
            }
        }
    }

    for (size_t i = 0; i < INA3221_POLICY_CHANNEL_COUNT; ++i) {
        const ina3221_overcurrent_channel_t *oc = &s_overcurrent.channels[i];
        portENTER_CRITICAL(&s_state_lock);
        s_rails[i].overcurrent_level = (int)oc->level;
        s_rails[i].overcurrent_latched = oc->latched;
        s_rails[i].overcurrent_latched_current_a = oc->latched_current_a;
        portEXIT_CRITICAL(&s_state_lock);
        (void)device_status_set_power_channel_overcurrent(
            i,
            (device_power_overcurrent_t)oc->level,
            oc->latched
        );
    }
}


static bool ina3221_monitor_stop_requested(void)
{
    bool requested;

    portENTER_CRITICAL(&s_state_lock);
    requested = s_stop_requested;
    portEXIT_CRITICAL(&s_state_lock);

    return requested;
}


static bool ina3221_monitor_fast_sampling_active(void)
{
    int64_t deadline_us;

    portENTER_CRITICAL(&s_state_lock);
    deadline_us = s_fast_sampling_deadline_us;
    portEXIT_CRITICAL(&s_state_lock);

    return esp_timer_get_time() < deadline_us;
}


static bool ina3221_monitor_register_current_task(void)
{
    const TaskHandle_t task =
        xTaskGetCurrentTaskHandle();

    bool stop_before_start;

    portENTER_CRITICAL(&s_state_lock);
    stop_before_start = s_stop_requested;
    s_task = task;
    s_starting = false;
    portEXIT_CRITICAL(&s_state_lock);

    return stop_before_start;
}


static void ina3221_monitor_clear_current_task(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_task = NULL;
    portEXIT_CRITICAL(&s_state_lock);
}


static device_power_source_t ina3221_monitor_device_source(
    ina3221_power_state_t state
)
{
    switch (state) {

        case INA3221_POWER_STATE_BATTERY:
            return DEVICE_POWER_SOURCE_BATTERY;

        case INA3221_POWER_STATE_USB:
        case INA3221_POWER_STATE_USB_CHARGING:
            return DEVICE_POWER_SOURCE_USB;

        case INA3221_POWER_STATE_UNKNOWN:
        default:
            return DEVICE_POWER_SOURCE_UNKNOWN;
    }
}


static void ina3221_monitor_publish_invalid(void)
{
    (void)device_status_set_power(
        0.0f,
        0.0f,
        false
    );

    for (
        size_t i = 0;
        i < INA3221_POLICY_CHANNEL_COUNT;
        ++i
    ) {
        (void)device_status_set_power_channel(
            i,
            0.0f,
            0.0f,
            false
        );
        ina3221_monitor_store_rail(i, false, 0.0f, 0.0f);
    }

    (void)device_status_set_power_battery(
        0.0f,
        0.0f,
        0.0f,
        false,
        false,
        false,
        false,
        DEVICE_POWER_SOURCE_UNKNOWN
    );
}


static void ina3221_monitor_publish(
    const ina3221_channel_sample_t
        channels[INA3221_POLICY_CHANNEL_COUNT],
    const ina3221_power_decision_t *decision
)
{
    for (
        size_t i = 0;
        i < INA3221_POLICY_CHANNEL_COUNT;
        ++i
    ) {
        (void)device_status_set_power_channel(
            i,
            channels[i].bus_voltage_v,
            channels[i].current_a,
            channels[i].valid
        );
        ina3221_monitor_store_rail(
            i, channels[i].valid, channels[i].bus_voltage_v, channels[i].current_a
        );
    }

    /*
     * The primary power values consumed by existing UI/status code are the
     * actual ESP32 5 V rail at physical INA3221 CH2 / IN- (after the R100
     * shunt). The status array index is the physical channel minus one.
     */
    (void)device_status_set_power(
        channels[1].bus_voltage_v,
        channels[1].current_a,
        channels[1].valid
    );

    (void)device_status_set_power_battery(
        decision->battery_voltage_v,
        decision->battery_current_a,
        decision->battery_percent_estimate,
        decision->battery_present,
        decision->battery_percent_valid,
        decision->usb_present,
        decision->charging,
        ina3221_monitor_device_source(
            decision->state
        )
    );
}

static device_main_height_reference_state_t ina3221_monitor_main_reference_state(
    main_barometer_reference_state_t state)
{
    switch (state) {
        case MAIN_BAROMETER_REFERENCE_VALID:
            return DEVICE_MAIN_HEIGHT_REFERENCE_VALID;
        case MAIN_BAROMETER_REFERENCE_REJECTED_UNSTABLE:
            return DEVICE_MAIN_HEIGHT_REFERENCE_REJECTED_UNSTABLE;
        case MAIN_BAROMETER_REFERENCE_SENSOR_INVALID:
            return DEVICE_MAIN_HEIGHT_REFERENCE_SENSOR_INVALID;
        case MAIN_BAROMETER_REFERENCE_NOT_ESTABLISHED:
        default:
            return DEVICE_MAIN_HEIGHT_REFERENCE_NOT_ESTABLISHED;
    }
}

static void ina3221_monitor_publish_main_barometer(main_barometer_t *barometer)
{
    main_barometer_reading_t reading = {0};
    main_barometer_get_reading(barometer, &reading);
    const device_main_status_t status = {
        .bmp280_present = reading.bmp280_present,
        .bmp280_valid = reading.bmp280_valid,
        .aht20_present = reading.aht20_present,
        .aht20_valid = reading.aht20_valid,
        .pressure_pa = reading.pressure_pa,
        .temperature_mc = reading.temperature_mc,
        .humidity_mpct = reading.humidity_mpct,
        .height_reference = {
            .state = ina3221_monitor_main_reference_state(
                reading.height_reference_state),
            .generation = reading.height_reference_generation,
            .relative_z_mm = reading.relative_z_mm,
        },
    };
    (void)device_status_set_main(&status);
}


static void ina3221_monitor_log_sample(
    const ina3221_channel_sample_t
        channels[INA3221_POLICY_CHANNEL_COUNT],
    const ina3221_power_decision_t *decision
)
{
    if (decision->battery_present) {
        ESP_LOGI(
            TAG,
            "state=%s source=%s INA_CH1_BATTERY=%.3fV %+.3fA %.0f%% soc=%s usb=%s charging=%s INA_CH2_ESP32_5V=%.3fV %+.3fA INA_CH3_AD7606_5V=%.3fV %+.3fA",
            ina3221_power_ux_state_name(s_ux.state),
            ina3221_policy_state_name(
                decision->state
            ),
            decision->battery_voltage_v,
            decision->battery_current_a,
            decision->battery_percent_estimate,
            ina3221_soc_quality_name(s_soc.quality),
            decision->usb_present ? "yes" : "no",
            decision->charging ? "yes" : "no",
            channels[1].bus_voltage_v,
            channels[1].current_a,
            channels[2].bus_voltage_v,
            channels[2].current_a
        );

        return;
    }

    ESP_LOGI(
        TAG,
        "state=%s source=%s INA_CH1_BATTERY=absent %.3fV %+.3fA usb=%s INA_CH2_ESP32_5V=%.3fV %+.3fA INA_CH3_AD7606_5V=%.3fV %+.3fA",
        ina3221_power_ux_state_name(s_ux.state),
        ina3221_policy_state_name(
            decision->state
        ),
        channels[0].bus_voltage_v,
        channels[0].current_a,
        decision->usb_present ? "yes" : "no",
        channels[1].bus_voltage_v,
        channels[1].current_a,
        channels[2].bus_voltage_v,
        channels[2].current_a
    );
}


static bool ina3221_monitor_wait_for_period_or_stop(void)
{
    const uint32_t period_ms =
        ina3221_monitor_fast_sampling_active() ?
            INA3221_MONITOR_FAST_PERIOD_MS :
            INA3221_MONITOR_PERIOD_MS;

    const uint32_t notifications =
        ulTaskNotifyTake(
            pdTRUE,
            pdMS_TO_TICKS(
                period_ms
            )
        );

    return
        notifications > 0U &&
        ina3221_monitor_stop_requested();
}


static void ina3221_monitor_worker(
    void *context
)
{
    (void)context;

    const bool stop_before_start =
        ina3221_monitor_register_current_task();

    ina3221_driver_t driver = { 0 };
    ina3221_power_classifier_t classifier = { 0 };

    ina3221_monitor_load_limits();
    ina3221_overcurrent_reset(&s_overcurrent);
    ina3221_monitor_load_power_ux();

    uint32_t sample_count = 0;
    uint32_t retry_count = 0;

    bool hardware_available =
        false;

    main_barometer_t main_barometer;
    main_barometer_init(&main_barometer);

    if (!stop_before_start) {
        while (true) {
            (void)main_barometer_poll(
                &main_barometer,
                (uint64_t)(esp_timer_get_time() / INT64_C(1000)));
            ina3221_monitor_publish_main_barometer(&main_barometer);

            if (!driver.ready) {
                const esp_err_t open_err =
                    ina3221_driver_open(
                        &driver
                    );

                if (open_err != ESP_OK) {
                    ina3221_monitor_publish_invalid();

                    retry_count++;

                    if (
                        retry_count == 1 ||
                        retry_count %
                            INA3221_RETRY_LOG_INTERVAL == 0
                    ) {
                        ESP_LOGW(
                            TAG,
                            "INA3221 unavailable at 0x40; retrying: %s",
                            esp_err_to_name(
                                open_err
                            )
                        );
                    }

                    hardware_available =
                        false;

                    if (ina3221_monitor_wait_for_period_or_stop()) {
                        break;
                    }
                    continue;
                }

                retry_count =
                    0;

                memset(
                    &classifier,
                    0,
                    sizeof(classifier)
                );

                /* MAX-245: CH1 hardware alert limits (CRI/WAR are not
                 * wired, so this is observability only); the software
                 * reaction below does not depend on it. */
                const esp_err_t limits_err =
                    ina3221_driver_program_limits(&driver, &s_limits);
                if (limits_err != ESP_OK) {
                    ESP_LOGW(
                        TAG,
                        "INA3221 alert limits not programmed: %s",
                        esp_err_to_name(limits_err)
                    );
                }

                if (!hardware_available) {
                    ESP_LOGI(
                        TAG,
                        "INA3221 online: I2C 0x40, 100 kHz, avg=16, conversion=1.1 ms, period=1 s"
                    );
                }

                hardware_available =
                    true;
            }

            ina3221_channel_sample_t
                channels[INA3221_POLICY_CHANNEL_COUNT] = { 0 };

            const esp_err_t read_err =
                ina3221_driver_read_channels(
                    &driver,
                    channels
                );

            if (read_err != ESP_OK) {
                ESP_LOGW(
                    TAG,
                    "INA3221 read failed; measurements invalidated: %s",
                    esp_err_to_name(
                        read_err
                    )
                );

                ina3221_monitor_publish_invalid();

                ina3221_driver_close(
                    &driver
                );

                memset(
                    &classifier,
                    0,
                    sizeof(classifier)
                );

                hardware_available =
                    false;

                if (ina3221_monitor_wait_for_period_or_stop()) {
                    break;
                }
                continue;
            }

            const ina3221_power_state_t instantaneous_state =
                ina3221_policy_classify_instant(
                    channels
                );

            const ina3221_power_state_t stable_state =
                ina3221_policy_classifier_update(
                    &classifier,
                    instantaneous_state
                );

            ina3221_power_decision_t decision;

            ina3221_policy_make_decision(
                channels,
                stable_state,
                &decision
            );

            ina3221_monitor_apply_soc(&decision);
            ina3221_monitor_log_evidence_changes(&decision);

            ina3221_monitor_publish(
                channels,
                &decision
            );

            ina3221_monitor_apply_overcurrent(channels);

            ina3221_monitor_apply_power_state(stable_state, &decision, true);

            sample_count++;

            if (
                sample_count == 1 ||
                sample_count %
                    INA3221_MONITOR_LOG_INTERVAL == 0 ||
                ina3221_monitor_fast_sampling_active()
            ) {
                ina3221_monitor_log_sample(
                    channels,
                    &decision
                );
            }

            if (ina3221_monitor_wait_for_period_or_stop()) {
                break;
            }
        }
    }

    main_barometer_close(&main_barometer);
    ina3221_driver_close(
        &driver
    );

    ina3221_monitor_clear_current_task();

    vTaskDelete(
        NULL
    );
}


esp_err_t ina3221_monitor_start(void)
{
    portENTER_CRITICAL(&s_state_lock);
    if (s_stop_requested) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_task != NULL || s_starting) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_OK;
    }
    s_starting = true;
    portEXIT_CRITICAL(&s_state_lock);

    TaskHandle_t task =
        NULL;

    const BaseType_t result =
        xTaskCreatePinnedToCore(
            ina3221_monitor_worker,
            "ina3221",
            APP_TASK_STACK_INA3221,
            NULL,
            APP_TASK_PRIORITY_INA3221,
            &task,
            APP_TASK_CORE_SERVICES
        );

    if (result != pdPASS) {
        portENTER_CRITICAL(&s_state_lock);
        s_starting = false;
        portEXIT_CRITICAL(&s_state_lock);

        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "Power monitor started on core=%d",
        APP_TASK_CORE_SERVICES
    );

    return ESP_OK;
}


void ina3221_monitor_request_fast_sampling(
    uint32_t duration_ms
)
{
    const int64_t deadline_us =
        esp_timer_get_time() +
        (int64_t)duration_ms * 1000;

    TaskHandle_t task;

    portENTER_CRITICAL(&s_state_lock);
    s_fast_sampling_deadline_us = deadline_us;
    task = s_task;
    portEXIT_CRITICAL(&s_state_lock);

    ESP_LOGI(
        TAG,
        "Fast sampling requested: 1/%u ms for %u ms",
        (unsigned)INA3221_MONITOR_FAST_PERIOD_MS,
        (unsigned)duration_ms
    );

    /*
     * The worker may already be blocked in ulTaskNotifyTake() with the old
     * 1 s period. Reuse the stop-notification channel to wake it early: a
     * notify with s_stop_requested still false just falls through to the
     * normal loop body, now observing the new deadline immediately instead
     * of up to 1 s late.
     */
    if (task != NULL) {
        (void)xTaskNotifyGive(
            task
        );
    }
}


void ina3221_monitor_stop(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_stop_requested = true;
    const TaskHandle_t task = s_task;
    const bool starting = s_starting;
    portEXIT_CRITICAL(&s_state_lock);

    if (task == NULL && !starting) {
        portENTER_CRITICAL(&s_state_lock);
        s_stop_requested = false;
        portEXIT_CRITICAL(&s_state_lock);
        return;
    }

    /*
     * A running worker only honors s_stop_requested after consuming this
     * notification, so the captured handle cannot disappear before this send.
     * If stop races task creation, there is no handle to notify: the worker
     * observes stop_before_start atomically when it registers itself and exits
     * without doing I2C work.
     */
    if (task != NULL) {
        (void)xTaskNotifyGive(
            task
        );
    }

    while (true) {
        portENTER_CRITICAL(&s_state_lock);
        const bool stopped =
            s_task == NULL &&
            !s_starting;
        portEXIT_CRITICAL(&s_state_lock);

        if (stopped) {
            break;
        }

        vTaskDelay(
            pdMS_TO_TICKS(10)
        );
    }

    portENTER_CRITICAL(&s_state_lock);
    s_stop_requested = false;
    portEXIT_CRITICAL(&s_state_lock);
}


void ina3221_monitor_set_critical_battery_handler(
    ina3221_monitor_critical_battery_handler_t handler
)
{
    portENTER_CRITICAL(&s_state_lock);
    s_critical_battery_handler = handler;
    portEXIT_CRITICAL(&s_state_lock);
}


esp_err_t ina3221_monitor_clear_overcurrent(
    size_t channel_index
)
{
    if (channel_index >= INA3221_POLICY_CHANNEL_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_state_lock);
    s_overcurrent_clear_mask |= (uint8_t)(1U << channel_index);
    s_rails[channel_index].overcurrent_level = 0;
    s_rails[channel_index].overcurrent_latched = false;
    s_rails[channel_index].overcurrent_latched_current_a = 0.0f;
    portEXIT_CRITICAL(&s_state_lock);
    (void)device_status_set_power_channel_overcurrent(
        channel_index, DEVICE_POWER_OVERCURRENT_NONE, false);
    return ESP_OK;
}


esp_err_t ina3221_monitor_get_rail(
    size_t channel_index,
    ina3221_monitor_rail_t *rail_out
)
{
    if (rail_out == NULL || channel_index >= INA3221_POLICY_CHANNEL_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_state_lock);
    *rail_out = s_rails[channel_index];
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

