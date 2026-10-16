#include "web_server_internal.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>

#include "esp_timer.h"
#include "ina3221_monitor.h"
#include "nrf_topology_service.h"
#include "runtime_observability.h"
#include "seismic_acquisition.h"
#include "seismic_buffer.h"
#include "storage_manager.h"
#include "web_status_policy.h"
#include "web_status_json_hardware.h"

bool web_status_json_add_workflow(cJSON *root);
#if defined(ESP_PLATFORM)
bool web_status_json_add_ranging(cJSON *root);
#endif

static struct
{
    char reason_name[24];
    int reason_code;
    uint32_t boot_id;
    uint64_t uptime_ms;
} s_reset_diagnostics = {
    .reason_name = "UNKNOWN",
};

void web_status_json_set_reset_diagnostics(
    const char *reason_name,
    int reason_code,
    uint32_t boot_id,
    uint64_t uptime_ms
)
{
    (void)snprintf(
        s_reset_diagnostics.reason_name,
        sizeof(s_reset_diagnostics.reason_name),
        "%s",
        reason_name != NULL ? reason_name : "UNKNOWN"
    );
    s_reset_diagnostics.reason_code = reason_code;
    s_reset_diagnostics.boot_id = boot_id;
    s_reset_diagnostics.uptime_ms = uptime_ms;
}

void web_server_https_runtime_status(
    uint32_t *clients_out,
    uint32_t *max_clients_out,
    uint32_t *handshake_failures_out,
    uint32_t *fatal_alerts_out,
    int32_t *last_error_esp_err_out
);

static size_t web_status_ready_pods(
    const device_status_snapshot_t *status,
    bool *has_error
)
{
    size_t ready = 0;
    bool error = false;

    for (size_t i = 0; i < DEVICE_STATUS_POD_COUNT; ++i) {
        if (status->pods[i].state == POD_STATE_READY) {
            ready++;
        }
        else if (status->pods[i].state == POD_STATE_ERROR) {
            error = true;
        }
    }

    if (has_error != NULL) {
        *has_error = error;
    }
    return ready;
}

static bool web_status_json_add_nrf_logs(cJSON *root)
{
    nrf_topology_snapshot_t topology;
    const bool topology_valid = nrf_topology_service_get_snapshot(&topology) == ESP_OK;
    cJSON *nodes = cJSON_AddArrayToObject(root, "nrf_logs");
    if (nodes == NULL) return false;

    for (uint8_t index = 0U; index < NRF_LOG_COLLECTOR_NODE_COUNT; ++index) {
        nrf_log_node_summary_t summary = {0};
        const bool have_summary = nrf_topology_service_get_nrf_log_summary(index, &summary) == ESP_OK;
        const bool present = topology_valid
            ? (index == 0U
                ? topology.main.state == NRF_TOPOLOGY_NODE_ASSIGNED
                : topology.remotes[index - 1U].state == NRF_TOPOLOGY_NODE_ASSIGNED)
            : (have_summary && summary.present);
        const char *node_name = index == 0U ? "main" : NULL;
        char remote_name[8];
        if (index != 0U) {
            (void)snprintf(remote_name, sizeof(remote_name), "remote%u", (unsigned)(index - 1U));
            node_name = remote_name;
        }

        cJSON *node = cJSON_CreateObject();
        if (node == NULL) return false;
        if (cJSON_AddItemToArray(nodes, node) == 0) {
            cJSON_Delete(node);
            return false;
        }
        if (
            cJSON_AddStringToObject(node, "node", node_name) == NULL ||
            (index == 0U
                ? cJSON_AddNullToObject(node, "channel") == NULL
                : cJSON_AddNumberToObject(node, "channel", index - 1U) == NULL) ||
            cJSON_AddBoolToObject(node, "present", present) == NULL ||
            cJSON_AddBoolToObject(node, "logs_unsupported", summary.logs_unsupported) == NULL ||
            (summary.boot_id_valid
                ? cJSON_AddNumberToObject(node, "boot_id", summary.boot_id) == NULL
                : cJSON_AddNullToObject(node, "boot_id") == NULL) ||
            cJSON_AddNumberToObject(node, "stored", summary.stored) == NULL ||
            cJSON_AddNumberToObject(node, "warn_count", summary.warn_count) == NULL ||
            cJSON_AddNumberToObject(node, "error_count", summary.error_count) == NULL ||
            cJSON_AddNumberToObject(node, "fatal_count", summary.fatal_count) == NULL ||
            cJSON_AddNumberToObject(node, "wire_gap_total", summary.wire_gap_total) == NULL ||
            cJSON_AddNumberToObject(node, "local_drop_total", summary.local_drop_total) == NULL ||
            cJSON_AddNumberToObject(node, "overwritten_total", summary.overwritten_total) == NULL) {
            return false;
        }
    }
    return true;
}

static bool web_status_json_add_system(
    cJSON *root,
    const device_status_snapshot_t *status,
    bool acquisition_running,
    bool acquisition_healthy,
    esp_err_t acquisition_error
)
{
    cJSON *system = cJSON_AddObjectToObject(root, "system");
    if (system == NULL) return false;

    bool pod_error = false;
    const size_t ready_pods = web_status_ready_pods(status, &pod_error);
    const web_system_state_t state = web_status_policy_evaluate(
        acquisition_running,
        status->acquisition.running,
        acquisition_healthy,
        acquisition_error != ESP_OK,
        ready_pods,
        pod_error,
        DEVICE_STATUS_POD_COUNT
    );

    const uint64_t uptime_ms = (uint64_t)(esp_timer_get_time() / 1000);
    return
        cJSON_AddStringToObject(system, "state", web_status_policy_name(state)) != NULL &&
        cJSON_AddStringToObject(system, "mode", device_status_mode_name(status->mode)) != NULL &&
        cJSON_AddNumberToObject(system, "uptime_ms", (double)uptime_ms) != NULL &&
        cJSON_AddStringToObject(system, "reset_reason", s_reset_diagnostics.reason_name) != NULL &&
        cJSON_AddBoolToObject(system, "acquisition_ready", acquisition_healthy) != NULL &&
        cJSON_AddNumberToObject(system, "ready_pods", (double)ready_pods) != NULL &&
        cJSON_AddNumberToObject(system, "required_pods", DEVICE_STATUS_POD_COUNT) != NULL;
}

static const char *web_status_power_channel_name(size_t index)
{
    static const char *const names[DEVICE_STATUS_POWER_CHANNEL_COUNT] = {
        "BATTERY",
        "ESP32_5V",
        "AD7606_5V",
    };

    return index < DEVICE_STATUS_POWER_CHANNEL_COUNT ? names[index] : "UNKNOWN";
}

static bool web_status_json_add_topology(cJSON *root)
{
    nrf_topology_snapshot_t snapshot = {0};
    const bool valid = nrf_topology_service_get_snapshot(&snapshot) == ESP_OK;
    cJSON *topology = cJSON_AddObjectToObject(root, "topology");
    if (topology == NULL) return false;
    return
        cJSON_AddBoolToObject(topology, "valid", valid) != NULL &&
        cJSON_AddNumberToObject(topology, "age_ms", valid ? snapshot.age_ms : 0U) != NULL &&
        cJSON_AddBoolToObject(topology, "stale", !valid || snapshot.stale) != NULL;
}

static bool web_status_json_add_power(cJSON *root, const device_power_status_t *power)
{
    cJSON *object = cJSON_AddObjectToObject(root, "power");
    if (object == NULL) return false;

    if (
        cJSON_AddBoolToObject(object, "valid", power->valid) == NULL ||
        cJSON_AddNumberToObject(object, "voltage_v", power->voltage_v) == NULL ||
        cJSON_AddNumberToObject(object, "current_a", power->current_a) == NULL ||
        cJSON_AddNumberToObject(object, "power_w", power->power_w) == NULL ||
        cJSON_AddStringToObject(
            object,
            "source",
            device_status_power_source_name(power->source)
        ) == NULL ||
        cJSON_AddBoolToObject(object, "battery_present", power->battery_present) == NULL ||
        cJSON_AddBoolToObject(object, "battery_percent_valid", power->battery_percent_valid) == NULL ||
        cJSON_AddNumberToObject(object, "battery_voltage_v", power->battery_voltage_v) == NULL ||
        cJSON_AddNumberToObject(object, "battery_current_a", power->battery_current_a) == NULL
    ) {
        return false;
    }
    const bool battery_percent_added = power->battery_percent_valid &&
        isfinite(power->battery_percent_estimate)
        ? cJSON_AddNumberToObject(object, "battery_percent_estimate", power->battery_percent_estimate) != NULL
        : cJSON_AddNullToObject(object, "battery_percent_estimate") != NULL;
    if (
        !battery_percent_added ||
        cJSON_AddBoolToObject(object, "usb_present", power->usb_present) == NULL ||
        cJSON_AddBoolToObject(object, "charging", power->charging) == NULL
    ) {
        return false;
    }
    /* MAX-180/183: user-facing state and the low-battery reaction. */
    if (
        cJSON_AddStringToObject(object, "state", device_status_power_state_name(power->state)) == NULL ||
        cJSON_AddBoolToObject(object, "battery_full", power->battery_full) == NULL ||
        cJSON_AddStringToObject(object, "low_battery", device_status_low_battery_name(power->low_battery)) == NULL ||
        cJSON_AddNumberToObject(object, "shutdown_in_s", (double)power->shutdown_in_s) == NULL ||
        /* MAX-181: provenance of battery_percent_estimate. */
        cJSON_AddStringToObject(
            object,
            "battery_soc_source",
            device_status_battery_soc_quality_name(power->battery_soc_quality)
        ) == NULL
    ) {
        return false;
    }

    cJSON *channels = cJSON_AddArrayToObject(object, "channels");
    if (channels == NULL) return false;
    for (size_t i = 0; i < DEVICE_STATUS_POWER_CHANNEL_COUNT; ++i) {
        const device_power_channel_status_t *channel = &power->channels[i];
        cJSON *channel_object = cJSON_CreateObject();
        if (channel_object == NULL) return false;
        if (!cJSON_AddItemToArray(channels, channel_object)) {
            cJSON_Delete(channel_object);
            return false;
        }
        /* MAX-245 software over-current state. */
        if (
            cJSON_AddNumberToObject(channel_object, "id", i + 1) == NULL ||
            cJSON_AddStringToObject(channel_object, "name", web_status_power_channel_name(i)) == NULL ||
            cJSON_AddBoolToObject(channel_object, "valid", channel->valid) == NULL ||
            cJSON_AddNumberToObject(channel_object, "voltage_v", channel->voltage_v) == NULL ||
            cJSON_AddNumberToObject(channel_object, "current_a", channel->current_a) == NULL ||
            cJSON_AddNumberToObject(channel_object, "power_w", channel->power_w) == NULL ||
            cJSON_AddStringToObject(
                channel_object,
                "overcurrent",
                device_status_power_overcurrent_name(channel->overcurrent)
            ) == NULL ||
            cJSON_AddBoolToObject(channel_object, "overcurrent_latched", channel->overcurrent_latched) == NULL
        ) {
            return false;
        }
        ina3221_monitor_rail_t rail = {0};
        const bool rail_valid = ina3221_monitor_get_rail(i, &rail) == ESP_OK;
        if (
            cJSON_AddNumberToObject(channel_object, "age_ms", rail_valid ? rail.age_ms : 0U) == NULL ||
            cJSON_AddBoolToObject(channel_object, "stale", !rail_valid || rail.stale) == NULL
        ) {
            return false;
        }
    }
    return true;
}

static bool web_status_json_add_environment(cJSON *root, const device_environment_status_t *environment)
{
    cJSON *object = cJSON_AddObjectToObject(root, "environment");
    if (object == NULL) return false;
    return
        cJSON_AddBoolToObject(object, "valid", environment->valid) != NULL &&
        cJSON_AddNumberToObject(object, "temperature_c", environment->temperature_c) != NULL &&
        cJSON_AddNumberToObject(object, "humidity_percent", environment->humidity_percent) != NULL &&
        cJSON_AddNumberToObject(object, "pressure_pa", environment->pressure_pa) != NULL;
}

static const char *web_status_main_height_reference_state_name(
    device_main_height_reference_state_t state)
{
    switch (state) {
        case DEVICE_MAIN_HEIGHT_REFERENCE_VALID: return "valid";
        case DEVICE_MAIN_HEIGHT_REFERENCE_REJECTED_UNSTABLE: return "rejected_unstable";
        case DEVICE_MAIN_HEIGHT_REFERENCE_SENSOR_INVALID: return "sensor_invalid";
        case DEVICE_MAIN_HEIGHT_REFERENCE_NOT_ESTABLISHED:
        default: return "not_established";
    }
}

static bool web_status_json_add_main(cJSON *root, const device_main_status_t *main_status)
{
    cJSON *object = cJSON_AddObjectToObject(root, "main");
    if (object == NULL) return false;

    cJSON *height_reference = cJSON_AddObjectToObject(object, "height_reference");
    cJSON *sensor_present = cJSON_AddObjectToObject(object, "sensor_present");
    cJSON *sensor_valid = cJSON_AddObjectToObject(object, "sensor_valid");
    if (height_reference == NULL || sensor_present == NULL || sensor_valid == NULL) return false;

    if (cJSON_AddStringToObject(
            height_reference, "state",
            web_status_main_height_reference_state_name(main_status->height_reference.state)) == NULL ||
        cJSON_AddNumberToObject(
            height_reference, "generation", main_status->height_reference.generation) == NULL ||
        cJSON_AddNumberToObject(
            height_reference, "relative_z_mm", main_status->height_reference.relative_z_mm) == NULL ||
        cJSON_AddNumberToObject(object, "pressure_pa", main_status->pressure_pa) == NULL ||
        cJSON_AddNumberToObject(object, "temperature_mc", main_status->temperature_mc) == NULL ||
        cJSON_AddBoolToObject(sensor_present, "bmp280", main_status->bmp280_present) == NULL ||
        cJSON_AddBoolToObject(sensor_present, "aht20", main_status->aht20_present) == NULL ||
        cJSON_AddBoolToObject(sensor_valid, "bmp280", main_status->bmp280_valid) == NULL ||
        cJSON_AddBoolToObject(sensor_valid, "aht20", main_status->aht20_valid) == NULL) {
        return false;
    }
    return !main_status->aht20_present ||
        cJSON_AddNumberToObject(object, "humidity_mpct", main_status->humidity_mpct) != NULL;
}

static bool web_status_json_add_mux(cJSON *root, const device_mux_status_t *mux)
{
    cJSON *object = cJSON_AddObjectToObject(root, "mux");
    if (object == NULL) return false;
    return
        cJSON_AddBoolToObject(object, "valid", mux->valid) != NULL &&
        cJSON_AddNumberToObject(object, "active_channel", mux->active_channel) != NULL &&
        cJSON_AddNumberToObject(object, "active_pod", mux->active_pod) != NULL;
}

static bool web_status_json_add_acquisition(
    cJSON *root,
    const device_acquisition_status_t *acquisition,
    bool running,
    bool hardware_ready,
    esp_err_t last_error,
    const seismic_acquisition_snapshot_t *source_snapshot,
    bool source_snapshot_available
)
{
    cJSON *object = cJSON_AddObjectToObject(root, "acquisition");
    if (object == NULL) return false;

    const bool base_ok =
        cJSON_AddBoolToObject(object, "running", running) != NULL &&
        cJSON_AddBoolToObject(object, "requested_running", acquisition->running) != NULL &&
        cJSON_AddBoolToObject(object, "hardware_ready", hardware_ready) != NULL &&
        cJSON_AddStringToObject(object, "source", seismic_acquisition_source_name()) != NULL &&
        cJSON_AddNumberToObject(object, "last_error", last_error) != NULL &&
        cJSON_AddStringToObject(object, "last_error_name", esp_err_to_name(last_error)) != NULL &&
        cJSON_AddNumberToObject(object, "sample_rate_hz", running ? acquisition->sample_rate_hz : 0) != NULL &&
        cJSON_AddNumberToObject(
            object,
            "os_ratio_configured",
            source_snapshot_available ? source_snapshot->os_ratio_configured : 0U
        ) != NULL &&
        cJSON_AddNumberToObject(
            object,
            "os_ratio_measured",
            source_snapshot_available ? source_snapshot->os_ratio_measured : 0U
        ) != NULL &&
        cJSON_AddBoolToObject(
            object,
            "os_mismatch",
            source_snapshot_available && source_snapshot->os_mismatch
        ) != NULL &&
        cJSON_AddNumberToObject(object, "samples_recorded", (double)acquisition->samples_recorded) != NULL &&
        cJSON_AddNumberToObject(object, "recording_ms", (double)acquisition->recording_ms) != NULL &&
        cJSON_AddBoolToObject(object, "error_counters_available", source_snapshot_available) != NULL &&
        cJSON_AddNumberToObject(
            object,
            "read_errors",
            source_snapshot_available ? (double)source_snapshot->read_errors : 0.0
        ) != NULL &&
        cJSON_AddNumberToObject(
            object,
            "consecutive_failures",
            source_snapshot_available ? (double)source_snapshot->consecutive_failures : 0.0
        ) != NULL;

    if (!base_ok) return false;

    cJSON *cadence = cJSON_AddObjectToObject(object, "cadence");
    if (cadence == NULL) return false;
    const bool cadence_available =
        source_snapshot_available && source_snapshot->cadence_available;
    if (
        cJSON_AddBoolToObject(cadence, "available", cadence_available) == NULL ||
        cJSON_AddNumberToObject(
            cadence,
            "timer_ticks",
            cadence_available ? (double)source_snapshot->timer_ticks : 0.0
        ) == NULL ||
        cJSON_AddNumberToObject(
            cadence,
            "conversion_attempts",
            cadence_available ? (double)source_snapshot->conversion_attempts : 0.0
        ) == NULL ||
        cJSON_AddNumberToObject(
            cadence,
            "collapsed_periods",
            cadence_available ? (double)source_snapshot->collapsed_periods : 0.0
        ) == NULL ||
        cJSON_AddNumberToObject(
            cadence,
            "last_successful_sample_interval_us",
            cadence_available
                ? (double)source_snapshot->last_successful_sample_interval_us
                : 0.0
        ) == NULL ||
        cJSON_AddNumberToObject(
            cadence,
            "max_successful_sample_interval_us",
            cadence_available
                ? (double)source_snapshot->max_successful_sample_interval_us
                : 0.0
        ) == NULL
    ) {
        return false;
    }

    const seismic_acquisition_jitter_t *jitter_source = cadence_available
        ? &source_snapshot->jitter
        : NULL;
    const uint32_t jitter_count = jitter_source != NULL
        ? jitter_source->count
        : 0U;
    const uint64_t jitter_sum_us = jitter_source != NULL
        ? jitter_source->sum_us
        : 0U;
    const double jitter_mean_us = jitter_count > 0U
        ? (double)jitter_sum_us / (double)jitter_count
        : 0.0;
    uint64_t jitter_p99_us = 0U;
    if (jitter_count > 0U) {
        /* Nearest-rank p99; bucket 15 is open-ended, represented by UINT64_MAX. */
        const uint64_t percentile_rank =
            ((uint64_t)jitter_count * 99U + 99U) / 100U;
        uint64_t cumulative = 0U;
        for (size_t bucket = 0U; bucket < SEISMIC_ACQUISITION_JITTER_BUCKET_COUNT; ++bucket) {
            cumulative += jitter_source->histogram_us[bucket];
            if (cumulative >= percentile_rank) {
                jitter_p99_us = bucket + 1U == SEISMIC_ACQUISITION_JITTER_BUCKET_COUNT
                    ? UINT64_MAX
                    : (UINT64_C(1) << (bucket + 1U));
                break;
            }
        }
    }

    cJSON *jitter = cJSON_AddObjectToObject(cadence, "jitter");
    if (jitter == NULL) return false;
    if (
        cJSON_AddNumberToObject(jitter, "count", jitter_count) == NULL ||
        cJSON_AddNumberToObject(
            jitter,
            "early_count",
            jitter_source != NULL ? (double)jitter_source->early_count : 0.0
        ) == NULL ||
        cJSON_AddNumberToObject(
            jitter,
            "min_us",
            jitter_source != NULL ? (double)jitter_source->min_us : 0.0
        ) == NULL ||
        cJSON_AddNumberToObject(
            jitter,
            "max_us",
            jitter_source != NULL ? (double)jitter_source->max_us : 0.0
        ) == NULL ||
        cJSON_AddNumberToObject(jitter, "mean_us", jitter_mean_us) == NULL ||
        cJSON_AddNumberToObject(jitter, "p99_us", (double)jitter_p99_us) == NULL ||
        cJSON_AddNumberToObject(
            jitter,
            "period_us",
            jitter_source != NULL ? (double)jitter_source->period_us : 0.0
        ) == NULL
    ) {
        return false;
    }

    cJSON *histogram = cJSON_AddArrayToObject(jitter, "histogram_us");
    if (histogram == NULL) return false;
    for (size_t bucket = 0U; bucket < SEISMIC_ACQUISITION_JITTER_BUCKET_COUNT; ++bucket) {
        const uint32_t bucket_count = jitter_source != NULL
            ? jitter_source->histogram_us[bucket]
            : 0U;
        cJSON *value = cJSON_CreateNumber(bucket_count);
        if (value == NULL) {
            return false;
        }
        if (!cJSON_AddItemToArray(histogram, value)) {
            cJSON_Delete(value);
            return false;
        }
    }

    cJSON *serial = cJSON_AddObjectToObject(object, "serial");
    if (serial == NULL) return false;

    const char *pattern =
        source_snapshot_available && source_snapshot->serial_pattern != NULL
            ? source_snapshot->serial_pattern
            : "UNKNOWN";
    const char *quality =
        source_snapshot_available && source_snapshot->serial_quality != NULL
            ? source_snapshot->serial_quality
            : "UNKNOWN";
    const char *stream_state =
        source_snapshot_available && source_snapshot->serial_stream_state != NULL
            ? source_snapshot->serial_stream_state
            : "UNKNOWN";
    const char *signal_quality =
        source_snapshot_available && source_snapshot->signal_quality != NULL
            ? source_snapshot->signal_quality
            : "UNKNOWN";
    const double quality_window = source_snapshot_available
        ? (double)source_snapshot->serial_quality_window_frames
        : 0.0;
    const double bad_ratio = quality_window > 0.0
        ? (double)source_snapshot->serial_quality_window_bad_frames / quality_window
        : 0.0;
    const double all_zero_ratio = quality_window > 0.0
        ? (double)source_snapshot->serial_quality_window_all_zero_frames / quality_window
        : 0.0;
    const double all_ff_ratio = quality_window > 0.0
        ? (double)source_snapshot->serial_quality_window_all_ff_frames / quality_window
        : 0.0;
    const double all_identical_ratio = quality_window > 0.0
        ? (double)source_snapshot->serial_quality_window_all_identical_frames / quality_window
        : 0.0;
    const double repeated_ratio = quality_window > 0.0
        ? (double)source_snapshot->serial_quality_window_repeated_frames / quality_window
        : 0.0;
    const double digital_ratio = quality_window > 0.0
        ? (double)source_snapshot->serial_quality_window_digital_interference_frames / quality_window
        : 0.0;
    const double signal_rail_ratio = quality_window > 0.0
        ? (double)source_snapshot->signal_quality_window_rail_hit_frames / quality_window
        : 0.0;
    const double signal_clip_ratio = quality_window > 0.0
        ? (double)source_snapshot->signal_quality_window_clip_frames / quality_window
        : 0.0;

    const bool serial_ok =
        cJSON_AddBoolToObject(
            serial,
            "suspicious",
            source_snapshot_available && source_snapshot->serial_suspicious
        ) != NULL &&
        cJSON_AddStringToObject(serial, "quality", quality) != NULL &&
        cJSON_AddStringToObject(serial, "stream_state", stream_state) != NULL &&
        cJSON_AddBoolToObject(
            serial,
            "stream_stuck",
            source_snapshot_available && source_snapshot->serial_stream_stuck
        ) != NULL &&
        /* MAX-242: AD7606 +5 V rail interlock. */
        cJSON_AddStringToObject(
            serial,
            "power_gate",
            source_snapshot_available && source_snapshot->power_gate != NULL
                ? source_snapshot->power_gate
                : "unknown"
        ) != NULL &&
        cJSON_AddBoolToObject(
            serial,
            "power_fault_latched",
            source_snapshot_available && source_snapshot->power_fault_latched
        ) != NULL &&
        cJSON_AddNumberToObject(
            serial,
            "power_fault_rail_v",
            source_snapshot_available ? (double)source_snapshot->power_fault_rail_v : 0.0
        ) != NULL &&
        /* MAX-245: rail current at an over-current fault. */
        cJSON_AddNumberToObject(
            serial,
            "power_fault_current_a",
            source_snapshot_available ? (double)source_snapshot->power_fault_current_a : 0.0
        ) != NULL &&
        cJSON_AddStringToObject(serial, "pattern", pattern) != NULL &&
        cJSON_AddNumberToObject(
            serial,
            "suspicious_streak",
            source_snapshot_available
                ? (double)source_snapshot->serial_suspicious_streak
                : 0.0
        ) != NULL &&
        cJSON_AddNumberToObject(
            serial,
            "repeated_frame_streak",
            source_snapshot_available
                ? (double)source_snapshot->serial_repeated_frame_streak
                : 0.0
        ) != NULL &&
        cJSON_AddNumberToObject(
            serial,
            "all_zero_frames",
            source_snapshot_available
                ? (double)source_snapshot->serial_all_zero_frames
                : 0.0
        ) != NULL &&
        cJSON_AddNumberToObject(
            serial,
            "all_ff_frames",
            source_snapshot_available
                ? (double)source_snapshot->serial_all_ff_frames
                : 0.0
        ) != NULL &&
        cJSON_AddNumberToObject(
            serial,
            "all_identical_frames",
            source_snapshot_available
                ? (double)source_snapshot->serial_all_identical_frames
                : 0.0
        ) != NULL &&
        cJSON_AddNumberToObject(
            serial,
            "varied_frames",
            source_snapshot_available
                ? (double)source_snapshot->serial_varied_frames
                : 0.0
        ) != NULL &&
        cJSON_AddNumberToObject(
            serial,
            "digital_interference_frames",
            source_snapshot_available
                ? (double)source_snapshot->serial_digital_interference_frames
                : 0.0
        ) != NULL &&
        cJSON_AddNumberToObject(
            serial,
            "quality_window_frames",
            quality_window
        ) != NULL &&
        cJSON_AddNumberToObject(serial, "quality_bad_ratio", bad_ratio) != NULL &&
        cJSON_AddNumberToObject(serial, "quality_all_00_ratio", all_zero_ratio) != NULL &&
        cJSON_AddNumberToObject(serial, "quality_all_ff_ratio", all_ff_ratio) != NULL &&
        cJSON_AddNumberToObject(serial, "quality_all_identical_ratio", all_identical_ratio) != NULL &&
        cJSON_AddNumberToObject(serial, "quality_repeated_ratio", repeated_ratio) != NULL &&
        cJSON_AddNumberToObject(serial, "quality_digital_interference_ratio", digital_ratio) != NULL &&
        cJSON_AddNumberToObject(
            serial,
            "busy_level",
            source_snapshot_available
                ? (double)source_snapshot->serial_busy_level
                : -1.0
        ) != NULL;

    if (!serial_ok) return false;

    cJSON *signal = cJSON_AddObjectToObject(object, "signal");
    if (signal == NULL) return false;

    return
        cJSON_AddStringToObject(signal, "quality", signal_quality) != NULL &&
        cJSON_AddNumberToObject(signal, "channel_count", SEISMIC_CHANNEL_COUNT) != NULL &&
        cJSON_AddNumberToObject(
            signal,
            "rail_hit_frames",
            source_snapshot_available
                ? (double)source_snapshot->signal_rail_hit_frames
                : 0.0
        ) != NULL &&
        cJSON_AddNumberToObject(
            signal,
            "clip_frames",
            source_snapshot_available
                ? (double)source_snapshot->signal_clip_frames
                : 0.0
        ) != NULL &&
        cJSON_AddNumberToObject(signal, "quality_window_frames", quality_window) != NULL &&
        cJSON_AddNumberToObject(signal, "rail_hit_ratio", signal_rail_ratio) != NULL &&
        cJSON_AddNumberToObject(signal, "clip_ratio", signal_clip_ratio) != NULL;
}

static bool web_status_json_add_runtime(cJSON *root)
{
    runtime_observability_snapshot_t snapshot;
    runtime_observability_get_snapshot(&snapshot);
    runtime_observability_heap_snapshot_t heap_snapshot;
    runtime_observability_get_heap_snapshot(&heap_snapshot);

    cJSON *runtime = cJSON_AddObjectToObject(root, "runtime");
    if (runtime == NULL) return false;
    if (cJSON_AddNumberToObject(runtime, "now_ms", (double)snapshot.now_ms) == NULL) {
        return false;
    }

    cJSON *heap = cJSON_AddObjectToObject(runtime, "heap");
    if (
        heap == NULL ||
        cJSON_AddNumberToObject(heap, "free_internal", (double)heap_snapshot.free_internal) == NULL ||
        cJSON_AddNumberToObject(heap, "largest_internal_block", (double)heap_snapshot.largest_internal_block) == NULL ||
        cJSON_AddNumberToObject(heap, "free_dma", (double)heap_snapshot.free_dma) == NULL ||
        cJSON_AddNumberToObject(heap, "largest_dma_block", (double)heap_snapshot.largest_dma_block) == NULL ||
        cJSON_AddNumberToObject(heap, "free_psram", (double)heap_snapshot.free_psram) == NULL ||
        cJSON_AddNumberToObject(heap, "min_free_internal_since_boot", (double)heap_snapshot.min_free_internal_since_boot) == NULL
    ) {
        return false;
    }

    bool mdns_ready = false;
    uint32_t mdns_retry_count = 0U;
    uint32_t mdns_next_retry_delay_ms = 0U;
    web_server_mdns_runtime_status(
        &mdns_ready,
        &mdns_retry_count,
        &mdns_next_retry_delay_ms
    );
    cJSON *mdns = cJSON_AddObjectToObject(runtime, "mdns");
    if (
        mdns == NULL ||
        cJSON_AddBoolToObject(mdns, "ready", mdns_ready) == NULL ||
        cJSON_AddNumberToObject(mdns, "retry_count", (double)mdns_retry_count) == NULL ||
        cJSON_AddBoolToObject(runtime, "mdns_ready", mdns_ready) == NULL ||
        cJSON_AddNumberToObject(runtime, "mdns_retry_count", (double)mdns_retry_count) == NULL ||
        cJSON_AddNumberToObject(runtime, "mdns_next_retry_delay_ms", (double)mdns_next_retry_delay_ms) == NULL
    ) {
        return false;
    }

    uint32_t https_clients = 0U;
    uint32_t https_max_clients = 0U;
    uint32_t https_handshake_failures = 0U;
    uint32_t https_fatal_alerts = 0U;
    int32_t https_last_error_esp_err = 0;
    web_server_https_runtime_status(
        &https_clients,
        &https_max_clients,
        &https_handshake_failures,
        &https_fatal_alerts,
        &https_last_error_esp_err
    );
    cJSON *https = cJSON_AddObjectToObject(runtime, "https");
    if (
        https == NULL ||
        cJSON_AddNumberToObject(https, "clients", (double)https_clients) == NULL ||
        cJSON_AddNumberToObject(https, "max_clients", (double)https_max_clients) == NULL ||
        cJSON_AddNumberToObject(https, "handshake_failures", (double)https_handshake_failures) == NULL ||
        cJSON_AddNumberToObject(https, "fatal_alerts", (double)https_fatal_alerts) == NULL ||
        cJSON_AddNumberToObject(https, "last_error_esp_err", (double)https_last_error_esp_err) == NULL
    ) {
        return false;
    }

    cJSON *tasks = cJSON_AddObjectToObject(runtime, "tasks");
    if (tasks == NULL) return false;

    for (int i = 0; i < RUNTIME_TASK_COUNT; ++i) {
        const runtime_observability_task_status_t *status = &snapshot.tasks[i];
        cJSON *task = cJSON_AddObjectToObject(
            tasks,
            runtime_observability_task_name((runtime_observability_task_t)i)
        );
        if (
            task == NULL ||
            cJSON_AddBoolToObject(task, "observed", status->observed) == NULL ||
            cJSON_AddNumberToObject(task, "heartbeat", (double)status->heartbeat) == NULL ||
            cJSON_AddNumberToObject(task, "progress", (double)status->progress) == NULL ||
            cJSON_AddNumberToObject(task, "last_heartbeat_ms", (double)status->last_heartbeat_ms) == NULL
        ) {
            return false;
        }
    }

    return true;
}

esp_err_t web_status_json_build_extended(
    char **json_out,
    bool include_objects,
    const char *sessions_object_id
)
{
    if (json_out == NULL) return ESP_ERR_INVALID_ARG;
    *json_out = NULL;

    const bool storage_available = storage_manager_is_ready();
    const bool storage_cache_ready = web_status_json_objects_cache_ready();
    if (
        (include_objects ||
         (sessions_object_id != NULL && sessions_object_id[0] != '\0')) &&
        !storage_cache_ready
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    device_status_snapshot_t status;
    esp_err_t err = device_status_get_snapshot(&status);
    if (err != ESP_OK) return err;

    const bool acquisition_running = seismic_acquisition_is_running();
    const bool acquisition_healthy = seismic_acquisition_source_healthy();
    const esp_err_t acquisition_error = seismic_acquisition_last_error();
    seismic_acquisition_snapshot_t acquisition_snapshot;
    const bool acquisition_snapshot_available =
        seismic_acquisition_get_snapshot(&acquisition_snapshot) == ESP_OK;

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return ESP_ERR_NO_MEM;

    if (cJSON_AddBoolToObject(root, "storage_available", storage_available) == NULL ||
        cJSON_AddBoolToObject(
            root,
            "storage_busy",
            storage_available && !storage_cache_ready
        ) == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    bool measurement_recording = false;
    const bool measurement_ok =
        web_status_json_add_measurement(root, &measurement_recording);

    const bool base_ok =
        web_status_json_add_system(
            root,
            &status,
            acquisition_running,
            acquisition_healthy,
            acquisition_error
        ) &&
        web_status_json_add_hardware(root) &&
        web_status_json_add_power(root, &status.power) &&
        web_status_json_add_topology(root) &&
        web_status_json_add_environment(root, &status.environment) &&
        web_status_json_add_main(root, &status.main) &&
        web_status_json_add_mux(root, &status.mux) &&
        web_status_json_add_pods(root, &status, measurement_recording) &&
        web_status_json_add_nrf_logs(root) &&
        web_status_json_add_acquisition(
            root,
            &status.acquisition,
            acquisition_running,
            acquisition_healthy,
            acquisition_error,
            &acquisition_snapshot,
            acquisition_snapshot_available
        ) &&
        web_status_json_add_runtime(root) &&
        web_status_json_add_recording(root) &&
        web_status_json_add_recording_gate(root) &&
        measurement_ok &&
        web_status_json_add_workflow(root);
#if defined(ESP_PLATFORM)
    const bool ranging_ok = base_ok && web_status_json_add_ranging(root);
#else
    const bool ranging_ok = base_ok;
#endif

    if (!ranging_ok) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    err = web_status_json_add_cache_sections(
        root,
        include_objects,
        sessions_object_id
    );
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return err;
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) return ESP_ERR_NO_MEM;

    *json_out = json;
    return ESP_OK;
}

esp_err_t web_status_json_build(char **json_out)
{
    return web_status_json_build_extended(json_out, false, NULL);
}

void web_status_json_free(char *json)
{
    if (json != NULL) cJSON_free(json);
}
