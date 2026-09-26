#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "device_status.h"
#include "runtime_observability.h"
#include "seismic_acquisition.h"
#include "web_server_internal.h"

static device_status_snapshot_t g_status;
static esp_err_t g_status_result;
static bool g_cache_ready;
static esp_err_t g_objects_result;
static esp_err_t g_sessions_result;
static bool g_objects_called;
static bool g_sessions_called;
static bool g_recording_called;
static int64_t g_now_us;

static bool g_acquisition_running;
static bool g_acquisition_healthy;
static esp_err_t g_acquisition_error;
static esp_err_t g_acquisition_snapshot_result;
static seismic_acquisition_snapshot_t g_acquisition_snapshot;
static runtime_observability_snapshot_t g_runtime_snapshot;
static runtime_observability_heap_snapshot_t g_heap_snapshot;
static bool g_mdns_ready;
static uint32_t g_mdns_retry_count;
static uint32_t g_mdns_next_retry_delay_ms;
static uint32_t g_https_clients;
static uint32_t g_https_max_clients;
static uint32_t g_https_handshake_failures;
static uint32_t g_https_fatal_alerts;
static int32_t g_https_last_error_esp_err;

static void reset_fakes(void)
{
    memset(&g_status, 0, sizeof(g_status));
    g_status_result = ESP_OK;
    g_cache_ready = true;
    g_objects_result = ESP_OK;
    g_sessions_result = ESP_OK;
    g_objects_called = false;
    g_sessions_called = false;
    g_recording_called = false;
    g_now_us = 1234567;

    g_acquisition_running = true;
    g_acquisition_healthy = true;
    g_acquisition_error = ESP_OK;
    g_acquisition_snapshot_result = ESP_OK;
    memset(&g_acquisition_snapshot, 0, sizeof(g_acquisition_snapshot));
    memset(&g_runtime_snapshot, 0, sizeof(g_runtime_snapshot));
    memset(&g_heap_snapshot, 0, sizeof(g_heap_snapshot));
    g_mdns_ready = false;
    g_mdns_retry_count = 0U;
    g_mdns_next_retry_delay_ms = 0U;
    g_https_clients = 0U;
    g_https_max_clients = 0U;
    g_https_handshake_failures = 0U;
    g_https_fatal_alerts = 0U;
    g_https_last_error_esp_err = 0;
}

esp_err_t device_status_get_snapshot(device_status_snapshot_t *snapshot)
{
    if (snapshot == NULL) return ESP_ERR_INVALID_ARG;
    if (g_status_result != ESP_OK) return g_status_result;
    *snapshot = g_status;
    return ESP_OK;
}

const char *device_status_mode_name(device_mode_t mode)
{
    switch (mode) {
        case DEVICE_MODE_MEASUREMENT: return "MEASUREMENT";
        case DEVICE_MODE_WIFI: return "WIFI";
        case DEVICE_MODE_IDLE: default: return "IDLE";
    }
}

const char *device_status_power_source_name(device_power_source_t source)
{
    switch (source) {
        case DEVICE_POWER_SOURCE_BATTERY: return "BATTERY";
        case DEVICE_POWER_SOURCE_USB: return "USB";
        case DEVICE_POWER_SOURCE_UNKNOWN: default: return "UNKNOWN";
    }
}

const char *device_status_battery_soc_quality_name(device_battery_soc_quality_t quality)
{
    return quality == DEVICE_BATTERY_SOC_COULOMB_TRACKED ? "COULOMB_TRACKED" : "OTHER";
}

const char *device_status_power_state_name(device_power_state_t state)
{
    switch (state) {
        case DEVICE_POWER_STATE_USB_ONLY_NO_BATTERY: return "USB_ONLY_NO_BATTERY";
        case DEVICE_POWER_STATE_USB_CHARGING: return "USB_CHARGING";
        case DEVICE_POWER_STATE_USB_BATTERY_READY: return "USB_BATTERY_READY";
        case DEVICE_POWER_STATE_BATTERY: return "BATTERY";
        case DEVICE_POWER_STATE_BATTERY_LOW: return "BATTERY_LOW";
        case DEVICE_POWER_STATE_BATTERY_CRITICAL: return "BATTERY_CRITICAL";
        case DEVICE_POWER_STATE_UNKNOWN: default: return "UNKNOWN";
    }
}

const char *device_status_low_battery_name(device_low_battery_phase_t phase)
{
    return phase == DEVICE_LOW_BATTERY_GRACE ? "grace" : "other";
}

const char *device_status_power_overcurrent_name(device_power_overcurrent_t overcurrent)
{
    switch (overcurrent) {
        case DEVICE_POWER_OVERCURRENT_NONE: return "none";
        case DEVICE_POWER_OVERCURRENT_WARNING: return "warning";
        case DEVICE_POWER_OVERCURRENT_CRITICAL: return "critical";
        default: return "unknown";
    }
}

const char *device_status_pod_name(pod_state_t state)
{
    switch (state) {
        case POD_STATE_READY: return "READY";
        case POD_STATE_WARNING: return "WARNING";
        case POD_STATE_ERROR: return "ERROR";
        case POD_STATE_DISCONNECTED: default: return "DISCONNECTED";
    }
}

bool seismic_acquisition_is_running(void)
{
    return g_acquisition_running;
}

bool seismic_acquisition_source_healthy(void)
{
    return g_acquisition_healthy;
}

esp_err_t seismic_acquisition_last_error(void)
{
    return g_acquisition_error;
}

const char *seismic_acquisition_source_name(void)
{
    return "HOST_FAKE_AD7606";
}

esp_err_t seismic_acquisition_get_snapshot(seismic_acquisition_snapshot_t *snapshot)
{
    if (snapshot == NULL) return ESP_ERR_INVALID_ARG;
    if (g_acquisition_snapshot_result != ESP_OK) return g_acquisition_snapshot_result;
    *snapshot = g_acquisition_snapshot;
    return ESP_OK;
}

void runtime_observability_get_snapshot(runtime_observability_snapshot_t *snapshot)
{
    assert(snapshot != NULL);
    *snapshot = g_runtime_snapshot;
}

void runtime_observability_get_heap_snapshot(runtime_observability_heap_snapshot_t *snapshot)
{
    assert(snapshot != NULL);
    *snapshot = g_heap_snapshot;
}

void web_server_mdns_runtime_status(
    bool *ready_out,
    uint32_t *retry_count_out,
    uint32_t *next_retry_delay_ms_out
)
{
    if (ready_out != NULL) *ready_out = g_mdns_ready;
    if (retry_count_out != NULL) *retry_count_out = g_mdns_retry_count;
    if (next_retry_delay_ms_out != NULL) *next_retry_delay_ms_out = g_mdns_next_retry_delay_ms;
}

void web_server_https_runtime_status(
    uint32_t *clients_out,
    uint32_t *max_clients_out,
    uint32_t *handshake_failures_out,
    uint32_t *fatal_alerts_out,
    int32_t *last_error_esp_err_out
)
{
    if (clients_out != NULL) *clients_out = g_https_clients;
    if (max_clients_out != NULL) *max_clients_out = g_https_max_clients;
    if (handshake_failures_out != NULL) *handshake_failures_out = g_https_handshake_failures;
    if (fatal_alerts_out != NULL) *fatal_alerts_out = g_https_fatal_alerts;
    if (last_error_esp_err_out != NULL) *last_error_esp_err_out = g_https_last_error_esp_err;
}

const char *runtime_observability_task_name(runtime_observability_task_t task)
{
    switch (task) {
        case RUNTIME_TASK_ACQUISITION: return "acquisition";
        case RUNTIME_TASK_RECORDER: return "recorder";
        case RUNTIME_TASK_UI: return "ui";
        case RUNTIME_TASK_COUNT: default: return "unknown";
    }
}

bool web_status_json_objects_cache_ready(void)
{
    return g_cache_ready;
}

esp_err_t web_status_json_add_objects(cJSON *root)
{
    g_objects_called = true;
    if (g_objects_result != ESP_OK) return g_objects_result;
    return cJSON_AddStringToObject(root, "object_store_marker", "objects-added") != NULL
        ? ESP_OK
        : ESP_ERR_NO_MEM;
}

esp_err_t web_status_json_add_sessions(cJSON *root, const char *object_id)
{
    g_sessions_called = true;
    if (g_sessions_result != ESP_OK) return g_sessions_result;
    return
        cJSON_AddStringToObject(root, "sessions_marker", object_id) != NULL
            ? ESP_OK
            : ESP_ERR_NO_MEM;
}

bool web_status_json_add_recording(cJSON *root)
{
    g_recording_called = true;
    return cJSON_AddStringToObject(root, "recording_marker", "recording-added") != NULL;
}

bool web_status_json_add_recording_gate(cJSON *root)
{
    return cJSON_AddStringToObject(root, "recording_gate_marker", "gate-added") != NULL;
}

bool web_status_json_add_measurement(cJSON *root)
{
    return cJSON_AddStringToObject(root, "measurement_marker", "measurement-added") != NULL;
}

int64_t esp_timer_get_time(void)
{
    return g_now_us;
}

static cJSON *object_item(const cJSON *object, const char *name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    assert(item != NULL);
    return item;
}

static const char *string_item(const cJSON *object, const char *name)
{
    cJSON *item = object_item(object, name);
    assert(cJSON_IsString(item));
    return item->valuestring;
}

static double number_item(const cJSON *object, const char *name)
{
    cJSON *item = object_item(object, name);
    assert(cJSON_IsNumber(item));
    return item->valuedouble;
}

static bool bool_item(const cJSON *object, const char *name)
{
    cJSON *item = object_item(object, name);
    assert(cJSON_IsBool(item));
    return cJSON_IsTrue(item);
}

static void assert_number_near(double actual, double expected)
{
    assert(fabs(actual - expected) < 0.000001);
}

static void fill_status(void)
{
    g_status.mode = DEVICE_MODE_MEASUREMENT;
    g_status.power.valid = true;
    g_status.power.voltage_v = 12.5f;
    g_status.power.current_a = 0.75f;
    g_status.power.power_w = 9.375f;
    g_status.power.source = DEVICE_POWER_SOURCE_USB;
    g_status.power.battery_present = true;
    g_status.power.battery_percent_valid = true;
    g_status.power.battery_voltage_v = 3.9f;
    g_status.power.battery_current_a = 0.2f;
    g_status.power.battery_percent_estimate = 64.0f;
    g_status.power.usb_present = true;
    g_status.power.charging = true;
    for (size_t i = 0; i < DEVICE_STATUS_POWER_CHANNEL_COUNT; ++i) {
        g_status.power.channels[i].valid = true;
        g_status.power.channels[i].voltage_v = (float)(i + 1U);
        g_status.power.channels[i].current_a = (float)(i + 2U);
        g_status.power.channels[i].power_w = (float)(i + 3U);
        g_status.power.channels[i].overcurrent = DEVICE_POWER_OVERCURRENT_NONE;
        g_status.power.channels[i].overcurrent_latched = false;
    }

    g_status.environment.valid = true;
    g_status.environment.temperature_c = 22.5f;
    g_status.environment.humidity_percent = 48.0f;
    g_status.environment.pressure_pa = 100321.0f;
    g_status.mux.valid = true;
    g_status.mux.active_channel = 3;
    g_status.mux.active_pod = 2;
    g_status.main.bmp280_present = true;
    g_status.main.bmp280_valid = true;
    g_status.main.aht20_present = true;
    g_status.main.aht20_valid = true;
    g_status.main.pressure_pa = 100654U;
    g_status.main.temperature_mc = 25080;
    g_status.main.humidity_mpct = 50000U;
    g_status.main.height_reference.state = DEVICE_MAIN_HEIGHT_REFERENCE_VALID;
    g_status.main.height_reference.generation = 27U;
    g_status.main.height_reference.relative_z_mm = 0;

    for (size_t i = 0; i < DEVICE_STATUS_POD_COUNT; ++i) {
        device_pod_status_t *pod = &g_status.pods[i];
        pod->state = POD_STATE_READY;
        pod->capabilities.barometer = true;
        pod->capabilities.environment = true;
        pod->capabilities.imu = true;
        pod->capabilities.geophone = true;
        pod->capabilities.local_position = true;
        pod->capabilities.global_position = true;
    }

    g_status.pods[0].height_reference.state = DEVICE_HEIGHT_REFERENCE_NOT_ESTABLISHED;
    g_status.pods[0].height_reference.generation = 0U;
    g_status.pods[0].height_reference.relative_z_mm = 0;
    g_status.pods[1].height_reference.state = DEVICE_HEIGHT_REFERENCE_VALID;
    g_status.pods[1].height_reference.generation = 27U;
    g_status.pods[1].height_reference.relative_z_mm = 1500;
    g_status.pods[2].height_reference.state = DEVICE_HEIGHT_REFERENCE_REJECTED_MOVING;
    g_status.pods[2].height_reference.generation = 27U;
    g_status.pods[2].height_reference.relative_z_mm = 0;
    g_status.pods[3].height_reference.state = DEVICE_HEIGHT_REFERENCE_SENSOR_INVALID;
    g_status.pods[3].height_reference.generation = 28U;
    g_status.pods[3].height_reference.relative_z_mm = 0;

    device_pod_status_t *pod = &g_status.pods[0];
    pod->barometer.valid = true;
    pod->barometer.pressure_pa = 100111.0f;
    pod->barometer.relative_altitude_cm = 12.5f;
    pod->barometer.reference_pressure_pa = 100000.0f;
    pod->environment.valid = true;
    pod->environment.temperature_valid = true;
    pod->environment.temperature_c = 23.25f;
    pod->environment.humidity_valid = true;
    pod->environment.humidity_percent = 51.0f;
    pod->imu.valid = true;
    pod->imu.roll_deg = 1.5f;
    pod->imu.pitch_deg = -2.5f;
    pod->imu.yaw_relative_deg = 33.0f;
    pod->geophone.valid = true;
    pod->geophone.instant_uv = 4.0f;
    pod->geophone.rms_uv = 2.0f;
    pod->geophone.peak_uv = 8.0f;
    pod->geophone.noise_rms_uv = 0.5f;
    pod->geophone.snr_db = 12.0f;
    pod->position.local_valid = true;
    pod->position.local_x_m = 1.25f;
    pod->position.local_y_m = -0.75f;
    pod->position.global_valid = true;
    pod->position.latitude = 50.4501;
    pod->position.longitude = 30.5234;
    pod->position.altitude_valid = true;
    pod->position.altitude_m = 179.2;

    g_status.acquisition.running = true;
    g_status.acquisition.sample_rate_hz = 4000;
    g_status.acquisition.samples_recorded = 987654;
    g_status.acquisition.recording_ms = 12345;

    g_acquisition_snapshot.running = true;
    g_acquisition_snapshot.source_healthy = true;
    g_acquisition_snapshot.latest_valid = true;
    g_acquisition_snapshot.sample_rate_hz = 4000;
    g_acquisition_snapshot.read_errors = 2;
    g_acquisition_snapshot.consecutive_failures = 1;
    g_acquisition_snapshot.cadence_available = true;
    g_acquisition_snapshot.timer_ticks = 1000;
    g_acquisition_snapshot.conversion_attempts = 998;
    g_acquisition_snapshot.collapsed_periods = 2;
    g_acquisition_snapshot.last_successful_sample_interval_us = 250;
    g_acquisition_snapshot.max_successful_sample_interval_us = 500;
    g_acquisition_snapshot.serial_suspicious = true;
    g_acquisition_snapshot.serial_pattern = "VARIED";
    g_acquisition_snapshot.serial_quality = "GOOD";
    g_acquisition_snapshot.serial_stream_state = "RUNNING";
    g_acquisition_snapshot.serial_stream_stuck = false;
    g_acquisition_snapshot.power_gate = "rail_low";
    g_acquisition_snapshot.power_fault_latched = true;
    g_acquisition_snapshot.power_fault_rail_v = 3.25f;
    g_acquisition_snapshot.power_fault_current_a = 0.42f;
    g_status.power.state = DEVICE_POWER_STATE_BATTERY_CRITICAL;
    g_status.power.low_battery = DEVICE_LOW_BATTERY_GRACE;
    g_status.power.shutdown_in_s = 7U;
    g_status.power.battery_soc_quality = DEVICE_BATTERY_SOC_COULOMB_TRACKED;
    g_status.power.battery_full = false;
    g_status.power.channels[2].overcurrent = DEVICE_POWER_OVERCURRENT_CRITICAL;
    g_status.power.channels[2].overcurrent_latched = true;
    g_acquisition_snapshot.signal_quality = "CLEAN";
    g_acquisition_snapshot.serial_suspicious_streak = 3;
    g_acquisition_snapshot.serial_repeated_frame_streak = 4;
    g_acquisition_snapshot.serial_all_zero_frames = 5;
    g_acquisition_snapshot.serial_all_ff_frames = 6;
    g_acquisition_snapshot.serial_all_identical_frames = 7;
    g_acquisition_snapshot.serial_varied_frames = 8;
    g_acquisition_snapshot.serial_digital_interference_frames = 9;
    g_acquisition_snapshot.signal_rail_hit_frames = 10;
    g_acquisition_snapshot.signal_clip_frames = 11;
    g_acquisition_snapshot.serial_quality_window_frames = 10;
    g_acquisition_snapshot.serial_quality_window_bad_frames = 2;
    g_acquisition_snapshot.serial_quality_window_all_zero_frames = 1;
    g_acquisition_snapshot.serial_quality_window_all_ff_frames = 3;
    g_acquisition_snapshot.serial_quality_window_all_identical_frames = 4;
    g_acquisition_snapshot.serial_quality_window_repeated_frames = 5;
    g_acquisition_snapshot.serial_quality_window_digital_interference_frames = 6;
    g_acquisition_snapshot.signal_quality_window_rail_hit_frames = 7;
    g_acquisition_snapshot.signal_quality_window_clip_frames = 8;
    g_acquisition_snapshot.serial_busy_level = 2;

    g_runtime_snapshot.now_ms = 4567;
    g_runtime_snapshot.tasks[RUNTIME_TASK_ACQUISITION].observed = true;
    g_runtime_snapshot.tasks[RUNTIME_TASK_ACQUISITION].heartbeat = 11;
    g_runtime_snapshot.tasks[RUNTIME_TASK_ACQUISITION].progress = 22;
    g_runtime_snapshot.tasks[RUNTIME_TASK_ACQUISITION].last_heartbeat_ms = 3333;
}

static cJSON *build_json(bool include_objects, const char *sessions_object_id)
{
    char *json = NULL;
    assert(web_status_json_build_extended(
        &json,
        include_objects,
        sessions_object_id
    ) == ESP_OK);
    assert(json != NULL);
    cJSON *root = cJSON_Parse(json);
    assert(root != NULL);
    web_status_json_free(json);
    return root;
}

static void test_full_builder_and_pods(void)
{
    reset_fakes();
    fill_status();
    g_mdns_ready = true;
    g_mdns_retry_count = 2U;
    g_mdns_next_retry_delay_ms = 5000U;
    cJSON *root = build_json(false, NULL);

    assert(!bool_item(root, "storage_busy"));
    cJSON *system = object_item(root, "system");
    assert(strcmp(string_item(system, "state"), "READY") == 0);
    assert(strcmp(string_item(system, "mode"), "MEASUREMENT") == 0);
    assert_number_near(number_item(system, "uptime_ms"), 1234.0);
    assert_number_near(number_item(system, "ready_pods"), 4.0);
    assert_number_near(number_item(system, "required_pods"), 4.0);

    cJSON *power = object_item(root, "power");
    assert(bool_item(power, "valid"));
    assert(strcmp(string_item(power, "source"), "USB") == 0);
    assert_number_near(number_item(power, "power_w"), 9.375);
    assert(bool_item(power, "battery_percent_valid"));
    assert_number_near(number_item(power, "battery_percent_estimate"), 64.0);
    assert(cJSON_GetArraySize(object_item(power, "channels")) == 3);
    assert(strcmp(string_item(power, "state"), "BATTERY_CRITICAL") == 0);
    assert(strcmp(string_item(power, "low_battery"), "grace") == 0);
    assert_number_near(number_item(power, "shutdown_in_s"), 7.0);
    assert(strcmp(string_item(power, "battery_soc_source"), "COULOMB_TRACKED") == 0);
    assert(!bool_item(power, "battery_full"));
    {
        cJSON *channels = object_item(power, "channels");
        cJSON *ad7606 = cJSON_GetArrayItem(channels, 2);
        assert(strcmp(string_item(ad7606, "name"), "AD7606_5V") == 0);
        assert(strcmp(string_item(ad7606, "overcurrent"), "critical") == 0);
        assert(bool_item(ad7606, "overcurrent_latched"));
        cJSON *battery = cJSON_GetArrayItem(channels, 0);
        assert(strcmp(string_item(battery, "overcurrent"), "none") == 0);
        assert(!bool_item(battery, "overcurrent_latched"));
    }

    cJSON *acquisition = object_item(root, "acquisition");
    assert(bool_item(acquisition, "running"));
    assert_number_near(number_item(acquisition, "sample_rate_hz"), 4000.0);
    assert_number_near(number_item(acquisition, "read_errors"), 2.0);
    cJSON *cadence = object_item(acquisition, "cadence");
    assert(bool_item(cadence, "available"));
    assert_number_near(number_item(cadence, "collapsed_periods"), 2.0);
    cJSON *serial = object_item(acquisition, "serial");
    assert(strcmp(string_item(serial, "quality"), "GOOD") == 0);
    assert_number_near(number_item(serial, "quality_bad_ratio"), 0.2);
    assert_number_near(number_item(serial, "quality_all_ff_ratio"), 0.3);
    assert_number_near(number_item(serial, "busy_level"), 2.0);
    /* MAX-242: the AD7606 power interlock reaches the status JSON. */
    assert(strcmp(string_item(serial, "power_gate"), "rail_low") == 0);
    assert(bool_item(serial, "power_fault_latched"));
    assert_number_near(number_item(serial, "power_fault_rail_v"), 3.25);
    assert_number_near(number_item(serial, "power_fault_current_a"), 0.42);
    cJSON *signal = object_item(acquisition, "signal");
    assert(strcmp(string_item(signal, "quality"), "CLEAN") == 0);
    assert_number_near(number_item(signal, "clip_ratio"), 0.8);

    cJSON *runtime = object_item(root, "runtime");
    assert_number_near(number_item(runtime, "now_ms"), 4567.0);
    cJSON *heap = object_item(runtime, "heap");
    assert_number_near(number_item(heap, "free_internal"), 0.0);
    assert_number_near(number_item(heap, "largest_internal_block"), 0.0);
    assert_number_near(number_item(heap, "free_dma"), 0.0);
    assert_number_near(number_item(heap, "largest_dma_block"), 0.0);
    assert_number_near(number_item(heap, "free_psram"), 0.0);
    assert_number_near(number_item(heap, "min_free_internal_since_boot"), 0.0);
    cJSON *mdns = object_item(runtime, "mdns");
    assert(bool_item(mdns, "ready"));
    assert_number_near(number_item(mdns, "retry_count"), 2.0);
    assert(bool_item(runtime, "mdns_ready"));
    assert_number_near(number_item(runtime, "mdns_retry_count"), 2.0);
    assert_number_near(number_item(runtime, "mdns_next_retry_delay_ms"), 5000.0);
    cJSON *https = object_item(runtime, "https");
    assert_number_near(number_item(https, "clients"), 0.0);
    assert_number_near(number_item(https, "max_clients"), 0.0);
    assert_number_near(number_item(https, "handshake_failures"), 0.0);
    assert_number_near(number_item(https, "fatal_alerts"), 0.0);
    assert_number_near(number_item(https, "last_error_esp_err"), 0.0);
    cJSON *runtime_acquisition = object_item(object_item(runtime, "tasks"), "acquisition");
    assert(bool_item(runtime_acquisition, "observed"));
    assert_number_near(number_item(runtime_acquisition, "progress"), 22.0);

    cJSON *pods = object_item(root, "pods");
    assert(cJSON_GetArraySize(pods) == DEVICE_STATUS_POD_COUNT);
    cJSON *pod = cJSON_GetArrayItem(pods, 0);
    assert(strcmp(string_item(pod, "state"), "READY") == 0);
    assert(bool_item(object_item(pod, "capabilities"), "global_position"));
    assert_number_near(number_item(object_item(pod, "geophone"), "snr_db"), 12.0);
    assert_number_near(number_item(object_item(pod, "position"), "latitude"), 50.4501);
    assert(bool_item(object_item(pod, "position"), "altitude_valid"));

    cJSON *main = object_item(root, "main");
    cJSON *main_reference = object_item(main, "height_reference");
    assert(strcmp(string_item(main_reference, "state"), "valid") == 0);
    assert_number_near(number_item(main_reference, "generation"), 27.0);
    assert_number_near(number_item(main_reference, "relative_z_mm"), 0.0);
    assert_number_near(number_item(main, "pressure_pa"), 100654.0);
    assert_number_near(number_item(main, "temperature_mc"), 25080.0);
    assert_number_near(number_item(main, "humidity_mpct"), 50000.0);
    assert(bool_item(object_item(main, "sensor_present"), "bmp280"));
    assert(bool_item(object_item(main, "sensor_present"), "aht20"));
    assert(bool_item(object_item(main, "sensor_valid"), "bmp280"));
    assert(bool_item(object_item(main, "sensor_valid"), "aht20"));

    static const char *const reference_states[] = {
        "not_established", "valid", "rejected_moving", "sensor_invalid",
    };
    static const double reference_generations[] = {0.0, 27.0, 27.0, 28.0};
    static const double relative_z_mm[] = {0.0, 1500.0, 0.0, 0.0};
    for (size_t index = 0U; index < DEVICE_STATUS_POD_COUNT; ++index) {
        cJSON *height_reference = object_item(cJSON_GetArrayItem(pods, (int)index), "height_reference");
        assert(strcmp(string_item(height_reference, "state"), reference_states[index]) == 0);
        assert_number_near(number_item(height_reference, "generation"), reference_generations[index]);
        assert_number_near(number_item(height_reference, "relative_z_mm"), relative_z_mm[index]);
    }

    assert(g_recording_called);
    cJSON_Delete(root);
}

static void test_invalid_battery_percentage_is_not_serialized_as_zero(void)
{
    reset_fakes();
    fill_status();
    g_status.power.battery_percent_valid = false;
    g_status.power.battery_percent_estimate = 0.0f;
    cJSON *root = build_json(false, NULL);
    cJSON *power = object_item(root, "power");
    assert(!bool_item(power, "battery_percent_valid"));
    assert(cJSON_IsNull(object_item(power, "battery_percent_estimate")));
    cJSON_Delete(root);

    g_status.power.battery_percent_valid = true;
    g_status.power.battery_percent_estimate = NAN;
    root = build_json(false, NULL);
    power = object_item(root, "power");
    assert(cJSON_IsNull(object_item(power, "battery_percent_estimate")));
    cJSON_Delete(root);
}

static void test_power_json_state_strings_remain_stable(void)
{
    static const struct {
        device_power_state_t state;
        const char *name;
    } states[] = {
        { DEVICE_POWER_STATE_USB_ONLY_NO_BATTERY, "USB_ONLY_NO_BATTERY" },
        { DEVICE_POWER_STATE_USB_CHARGING, "USB_CHARGING" },
        { DEVICE_POWER_STATE_USB_BATTERY_READY, "USB_BATTERY_READY" },
        { DEVICE_POWER_STATE_BATTERY, "BATTERY" },
        { DEVICE_POWER_STATE_BATTERY_LOW, "BATTERY_LOW" },
        { DEVICE_POWER_STATE_BATTERY_CRITICAL, "BATTERY_CRITICAL" },
        { DEVICE_POWER_STATE_UNKNOWN, "UNKNOWN" },
    };
    for (size_t i = 0U; i < sizeof(states) / sizeof(states[0]); ++i) {
        reset_fakes();
        fill_status();
        g_status.power.state = states[i].state;
        cJSON *root = build_json(false, NULL);
        assert(strcmp(string_item(object_item(root, "power"), "state"), states[i].name) == 0);
        cJSON_Delete(root);
    }
}

static void test_main_status_omits_aht_humidity_when_absent(void)
{
    reset_fakes();
    fill_status();
    g_status.main.aht20_present = false;
    g_status.main.aht20_valid = false;
    cJSON *root = build_json(false, NULL);
    cJSON *main = object_item(root, "main");
    assert(cJSON_GetObjectItemCaseSensitive(main, "humidity_mpct") == NULL);
    assert(!bool_item(object_item(main, "sensor_present"), "aht20"));
    cJSON_Delete(root);
}

static void test_runtime_metrics_accept_zero_and_max_values(void)
{
    reset_fakes();
    fill_status();
    g_heap_snapshot = (runtime_observability_heap_snapshot_t) {
        .free_internal = SIZE_MAX,
        .largest_internal_block = SIZE_MAX,
        .free_dma = SIZE_MAX,
        .largest_dma_block = SIZE_MAX,
        .free_psram = SIZE_MAX,
        .min_free_internal_since_boot = SIZE_MAX,
    };
    g_mdns_ready = false;
    g_mdns_retry_count = UINT32_MAX;
    g_https_clients = UINT32_MAX;
    g_https_max_clients = UINT32_MAX;
    g_https_handshake_failures = UINT32_MAX;
    g_https_fatal_alerts = UINT32_MAX;
    g_https_last_error_esp_err = INT32_MAX;

    cJSON *root = build_json(false, NULL);
    cJSON *runtime = object_item(root, "runtime");
    cJSON *heap = object_item(runtime, "heap");
    assert(number_item(heap, "free_internal") == (double)SIZE_MAX);
    assert(number_item(heap, "largest_internal_block") == (double)SIZE_MAX);
    assert(number_item(heap, "free_dma") == (double)SIZE_MAX);
    assert(number_item(heap, "largest_dma_block") == (double)SIZE_MAX);
    assert(number_item(heap, "free_psram") == (double)SIZE_MAX);
    assert(number_item(heap, "min_free_internal_since_boot") == (double)SIZE_MAX);
    cJSON *mdns = object_item(runtime, "mdns");
    assert(!bool_item(mdns, "ready"));
    assert(number_item(mdns, "retry_count") == (double)UINT32_MAX);
    cJSON *https = object_item(runtime, "https");
    assert(number_item(https, "clients") == (double)UINT32_MAX);
    assert(number_item(https, "max_clients") == (double)UINT32_MAX);
    assert(number_item(https, "handshake_failures") == (double)UINT32_MAX);
    assert(number_item(https, "fatal_alerts") == (double)UINT32_MAX);
    assert(number_item(https, "last_error_esp_err") == (double)INT32_MAX);
    cJSON_Delete(root);
}

static void test_policy_fault_and_snapshot_fallback(void)
{
    reset_fakes();
    fill_status();
    g_status.pods[3].state = POD_STATE_ERROR;
    cJSON *root = build_json(false, NULL);
    assert(strcmp(string_item(object_item(root, "system"), "state"), "FAULT") == 0);
    cJSON_Delete(root);

    reset_fakes();
    g_status.acquisition.running = false;
    g_acquisition_running = false;
    g_acquisition_healthy = false;
    g_acquisition_error = ESP_FAIL;
    g_acquisition_snapshot_result = ESP_FAIL;
    root = build_json(false, NULL);
    cJSON *acquisition = object_item(root, "acquisition");
    assert(!bool_item(acquisition, "running"));
    assert(!bool_item(acquisition, "error_counters_available"));
    assert(strcmp(string_item(object_item(acquisition, "serial"), "pattern"), "UNKNOWN") == 0);
    assert_number_near(number_item(object_item(acquisition, "serial"), "busy_level"), -1.0);
    assert(strcmp(string_item(object_item(acquisition, "serial"), "power_gate"), "unknown") == 0);
    assert(!bool_item(object_item(acquisition, "serial"), "power_fault_latched"));
    assert(!bool_item(object_item(acquisition, "cadence"), "available"));
    cJSON_Delete(root);
}

static void test_extended_cache_and_errors(void)
{
    reset_fakes();
    fill_status();
    cJSON *root = build_json(true, "OBJ-1");
    assert(g_objects_called);
    assert(g_sessions_called);
    assert(strcmp(string_item(root, "object_store_marker"), "objects-added") == 0);
    assert(strcmp(string_item(root, "sessions_marker"), "OBJ-1") == 0);
    cJSON_Delete(root);

    reset_fakes();
    fill_status();
    g_cache_ready = false;
    char *json = NULL;
    assert(web_status_json_build_extended(&json, true, NULL) == ESP_ERR_INVALID_STATE);
    assert(json == NULL);

    assert(web_status_json_build_extended(NULL, false, NULL) == ESP_ERR_INVALID_ARG);

    reset_fakes();
    fill_status();
    g_status_result = ESP_FAIL;
    assert(web_status_json_build_extended(&json, false, NULL) == ESP_FAIL);
    assert(json == NULL);

    reset_fakes();
    fill_status();
    g_objects_result = ESP_ERR_INVALID_STATE;
    assert(web_status_json_build_extended(&json, true, NULL) == ESP_ERR_INVALID_STATE);
    assert(json == NULL);
    test_runtime_metrics_accept_zero_and_max_values();
}

int main(void)
{
    test_full_builder_and_pods();
    test_invalid_battery_percentage_is_not_serialized_as_zero();
    test_power_json_state_strings_remain_stable();
    test_main_status_omits_aht_humidity_when_absent();
    test_policy_fault_and_snapshot_fallback();
    test_extended_cache_and_errors();
    puts("web_status_json_builder_test: PASS");
    return 0;
}
