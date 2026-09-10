#include "web_server_internal.h"

#include <stddef.h>

#include "esp_timer.h"
#include "runtime_observability.h"
#include "seismic_acquisition.h"
#include "web_status_policy.h"

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

static bool web_status_json_add_power(cJSON *root, const device_power_status_t *power)
{
    cJSON *object = cJSON_AddObjectToObject(root, "power");
    if (object == NULL) return false;

    cJSON_AddBoolToObject(object, "valid", power->valid);
    cJSON_AddNumberToObject(object, "voltage_v", power->voltage_v);
    cJSON_AddNumberToObject(object, "current_a", power->current_a);
    cJSON_AddNumberToObject(object, "power_w", power->power_w);

    cJSON_AddStringToObject(
        object,
        "source",
        device_status_power_source_name(power->source)
    );
    cJSON_AddBoolToObject(object, "battery_present", power->battery_present);
    cJSON_AddBoolToObject(object, "battery_percent_valid", power->battery_percent_valid);
    cJSON_AddNumberToObject(object, "battery_voltage_v", power->battery_voltage_v);
    cJSON_AddNumberToObject(object, "battery_current_a", power->battery_current_a);
    cJSON_AddNumberToObject(object, "battery_percent_estimate", power->battery_percent_estimate);
    cJSON_AddBoolToObject(object, "usb_present", power->usb_present);
    cJSON_AddBoolToObject(object, "charging", power->charging);

    cJSON *channels = cJSON_AddArrayToObject(object, "channels");
    if (channels == NULL) return false;
    for (size_t i = 0; i < DEVICE_STATUS_POWER_CHANNEL_COUNT; ++i) {
        const device_power_channel_status_t *channel = &power->channels[i];
        cJSON *channel_object = cJSON_CreateObject();
        if (channel_object == NULL) return false;
        cJSON_AddItemToArray(channels, channel_object);
        cJSON_AddNumberToObject(channel_object, "id", i + 1);
        cJSON_AddStringToObject(channel_object, "name", web_status_power_channel_name(i));
        cJSON_AddBoolToObject(channel_object, "valid", channel->valid);
        cJSON_AddNumberToObject(channel_object, "voltage_v", channel->voltage_v);
        cJSON_AddNumberToObject(channel_object, "current_a", channel->current_a);
        cJSON_AddNumberToObject(channel_object, "power_w", channel->power_w);
    }
    return true;
}

static bool web_status_json_add_environment(cJSON *root, const device_environment_status_t *environment)
{
    cJSON *object = cJSON_AddObjectToObject(root, "environment");
    if (object == NULL) return false;
    cJSON_AddBoolToObject(object, "valid", environment->valid);
    cJSON_AddNumberToObject(object, "temperature_c", environment->temperature_c);
    cJSON_AddNumberToObject(object, "humidity_percent", environment->humidity_percent);
    cJSON_AddNumberToObject(object, "pressure_pa", environment->pressure_pa);
    return true;
}

static bool web_status_json_add_mux(cJSON *root, const device_mux_status_t *mux)
{
    cJSON *object = cJSON_AddObjectToObject(root, "mux");
    if (object == NULL) return false;
    cJSON_AddBoolToObject(object, "valid", mux->valid);
    cJSON_AddNumberToObject(object, "active_channel", mux->active_channel);
    cJSON_AddNumberToObject(object, "active_pod", mux->active_pod);
    return true;
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

static bool web_status_json_add_runtime(
    cJSON *root
)
{
    runtime_observability_snapshot_t snapshot;
    runtime_observability_get_snapshot(&snapshot);

    cJSON *runtime = cJSON_AddObjectToObject(root, "runtime");
    if (runtime == NULL) return false;

    if (cJSON_AddNumberToObject(runtime, "now_ms", (double)snapshot.now_ms) == NULL) {
        return false;
    }

    cJSON *tasks = cJSON_AddObjectToObject(runtime, "tasks");
    if (tasks == NULL) return false;

    for (int i = 0; i < RUNTIME_TASK_COUNT; ++i) {
        const runtime_observability_task_status_t *status =
            &snapshot.tasks[i];
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

    bool ok =
        web_status_json_add_system(
            root,
            &status,
            acquisition_running,
            acquisition_healthy,
            acquisition_error
        ) &&
        web_status_json_add_power(root, &status.power) &&
        web_status_json_add_environment(root, &status.environment) &&
        web_status_json_add_mux(root, &status.mux) &&
        web_status_json_add_pods(root, &status) &&
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
        web_status_json_add_recording(root);

    if (ok && include_objects) {
        ok = web_status_json_add_objects(root);
    }

    if (ok && sessions_object_id != NULL && sessions_object_id[0] != '\0') {
        ok = web_status_json_add_sessions(root, sessions_object_id);
    }

    if (!ok) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
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
