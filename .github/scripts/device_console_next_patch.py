from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if new in text:
        return text
    if old not in text:
        raise SystemExit(f'Expected fragment not found for {label}:\n{old}')
    return text.replace(old, new, 1)


# 1) Fix the real ESP-IDF -Werror found by the full firmware build.
recorder_path = Path('components/seismic_recorder/seismic_recorder_fs.c')
recorder = recorder_path.read_text(encoding='utf-8')
recorder_original = recorder
recorder = replace_once(
    recorder,
    '''                snprintf(
                    summary.session_id,
                    sizeof(summary.session_id),
                    "%s",
                    entry->d_name
                );
                summary.complete = false;
''',
    '''                const size_t session_id_length = strnlen(
                    entry->d_name,
                    sizeof(summary.session_id)
                );
                if (session_id_length >= sizeof(summary.session_id)) {
                    continue;
                }
                memcpy(
                    summary.session_id,
                    entry->d_name,
                    session_id_length + 1U
                );
                summary.complete = false;
''',
    'recorder bounded session id copy',
)
if recorder != recorder_original:
    recorder_path.write_text(recorder, encoding='utf-8')
    print('Fixed recorder bounded session-id fallback copy.')


# 2) Object list: read global cable defaults once per list operation.
# The previous perf patch has already moved the large scratch buffers/summary
# array off the service task stack before this script runs.
profile_path = Path('components/object_store/object_store_profile.c')
profile = profile_path.read_text(encoding='utf-8')
profile_original = profile
profile = replace_once(
    profile,
    '''static void object_store_read_cables(cJSON *root, float lengths[OBJECT_STORE_POD_COUNT])
{
    if (object_store_settings_load_locked(lengths) != ESP_OK) {
        object_store_default_cables(lengths);
    }

    cJSON *pods = cJSON_GetObjectItemCaseSensitive(root, "pods");
''',
    '''static void object_store_read_cables(
    cJSON *root,
    const float defaults[OBJECT_STORE_POD_COUNT],
    float lengths[OBJECT_STORE_POD_COUNT]
)
{
    memcpy(
        lengths,
        defaults,
        sizeof(float) * OBJECT_STORE_POD_COUNT
    );

    cJSON *pods = cJSON_GetObjectItemCaseSensitive(root, "pods");
''',
    'object cables consume supplied defaults',
)
profile = replace_once(
    profile,
    '''esp_err_t object_store_json_read_profile_locked(
    const char *object_id,
    bool active,
    object_store_profile_t *profile
)
{
    if (profile == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = NULL;
    esp_err_t err = object_store_load_metadata_locked(object_id, &root);
    if (err != ESP_OK) {
        return err;
    }

    memset(profile, 0, sizeof(*profile));
    snprintf(profile->summary.id, sizeof(profile->summary.id), "%s", object_id);
    object_store_copy_name(root, object_id, profile->summary.name, sizeof(profile->summary.name));
    profile->summary.active = active;
    object_store_read_location(root, &profile->location);
    object_store_read_cables(root, profile->cable_lengths_m);
    cJSON_Delete(root);
    return ESP_OK;
}
''',
    '''static esp_err_t object_store_json_read_profile_with_defaults_locked(
    const char *object_id,
    bool active,
    const float defaults[OBJECT_STORE_POD_COUNT],
    object_store_profile_t *profile
)
{
    if (profile == NULL || defaults == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = NULL;
    esp_err_t err = object_store_load_metadata_locked(object_id, &root);
    if (err != ESP_OK) {
        return err;
    }

    memset(profile, 0, sizeof(*profile));
    snprintf(profile->summary.id, sizeof(profile->summary.id), "%s", object_id);
    object_store_copy_name(root, object_id, profile->summary.name, sizeof(profile->summary.name));
    profile->summary.active = active;
    object_store_read_location(root, &profile->location);
    object_store_read_cables(root, defaults, profile->cable_lengths_m);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t object_store_json_read_profile_locked(
    const char *object_id,
    bool active,
    object_store_profile_t *profile
)
{
    float defaults[OBJECT_STORE_POD_COUNT];
    if (object_store_settings_load_locked(defaults) != ESP_OK) {
        object_store_default_cables(defaults);
    }

    return object_store_json_read_profile_with_defaults_locked(
        object_id,
        active,
        defaults,
        profile
    );
}
''',
    'profile helper with preloaded defaults',
)
profile = replace_once(
    profile,
    '''    err = object_store_fs_list(summaries, bounded, &count, object_store_internal_active_id());

    if (err == ESP_OK) {
        for (size_t i = 0; i < count; ++i) {
            err = object_store_json_read_profile_locked(
                summaries[i].id,
                summaries[i].active,
                &objects[i]
            );
            if (err != ESP_OK) break;
        }
    }
''',
    '''    err = object_store_fs_list(summaries, bounded, &count, object_store_internal_active_id());

    float defaults[OBJECT_STORE_POD_COUNT];
    if (object_store_settings_load_locked(defaults) != ESP_OK) {
        object_store_default_cables(defaults);
    }

    if (err == ESP_OK) {
        for (size_t i = 0; i < count; ++i) {
            err = object_store_json_read_profile_with_defaults_locked(
                summaries[i].id,
                summaries[i].active,
                defaults,
                &objects[i]
            );
            if (err != ESP_OK) break;
        }
    }
''',
    'single defaults read for object list',
)
if profile != profile_original:
    profile_path.write_text(profile, encoding='utf-8')
    print('Collapsed object-list cable defaults to one SD/settings read.')


# 3) One /assets/* owner only; remove the dead four-handler implementation and
# shrink the HTTPS URI table from 14 to the ten handlers actually registered.
internal_path = Path('components/web_server/web_server_internal.h')
internal = internal_path.read_text(encoding='utf-8')
internal_original = internal
internal = internal.replace(
    'esp_err_t web_static_assets_register(httpd_handle_t server);\n',
    '',
)
if internal != internal_original:
    internal_path.write_text(internal, encoding='utf-8')
    print('Removed dead legacy static-handler declaration.')

cmake_path = Path('components/web_server/CMakeLists.txt')
cmake = cmake_path.read_text(encoding='utf-8')
cmake_original = cmake
cmake = cmake.replace('        "web_static_assets.c"\n', '')
if cmake != cmake_original:
    cmake_path.write_text(cmake, encoding='utf-8')
    print('Removed dead four-handler static asset translation unit from firmware.')

legacy_static_path = Path('components/web_server/web_static_assets.c')
if legacy_static_path.exists():
    legacy_static_path.unlink()
    print('Deleted dead exact-route legacy asset server.')

server_path = Path('components/web_server/web_server.c')
server = server_path.read_text(encoding='utf-8')
server_original = server
server = replace_once(
    server,
    '''    config.httpd.max_uri_handlers =
        14;
''',
    '''    /*
     * Exact HTTPS route budget after consolidating dashboard/seismic assets
     * under /assets/*: root, client-log, assets, status, control, seismic page,
     * seismic API, verify page, verify API, verify UI = 10 handlers.
     */
    config.httpd.max_uri_handlers =
        10;
''',
    'HTTPS handler table shrink',
)
if server != server_original:
    server_path.write_text(server, encoding='utf-8')
    print('Shrank HTTPS URI handler table from 14 to 10 entries.')


# 4) Future-proof pod status without fabricating nRF54L15 coordinates.
status_header_path = Path('components/device_status/include/device_status.h')
status_header = status_header_path.read_text(encoding='utf-8')
status_header_original = status_header
status_header = replace_once(
    status_header,
    '''typedef struct
{
    bool valid;

    float roll_deg;
    float pitch_deg;
    float yaw_relative_deg;

} device_imu_status_t;
''',
    '''typedef struct
{
    bool valid;

    float roll_deg;
    float pitch_deg;
    float yaw_relative_deg;

} device_imu_status_t;


typedef struct
{
    bool valid;
    bool temperature_valid;
    bool humidity_valid;

    float temperature_c;
    float humidity_percent;

} device_pod_environment_status_t;


typedef struct
{
    bool local_valid;
    float local_x_m;
    float local_y_m;

    bool global_valid;
    double latitude;
    double longitude;

    bool altitude_valid;
    double altitude_m;

} device_pod_position_status_t;


typedef struct
{
    bool barometer;
    bool environment;
    bool imu;
    bool geophone;
    bool local_position;
    bool global_position;

} device_pod_capabilities_t;
''',
    'pod environment position capability types',
)
status_header = replace_once(
    status_header,
    '''typedef struct
{
    pod_state_t state;

    device_barometer_status_t barometer;
    device_imu_status_t imu;
    device_geophone_status_t geophone;

} device_pod_status_t;
''',
    '''typedef struct
{
    pod_state_t state;

    device_pod_capabilities_t capabilities;
    device_barometer_status_t barometer;
    device_pod_environment_status_t environment;
    device_imu_status_t imu;
    device_geophone_status_t geophone;
    device_pod_position_status_t position;

} device_pod_status_t;
''',
    'pod aggregate expanded model',
)
status_header = replace_once(
    status_header,
    '''esp_err_t device_status_set_pod_imu(
    size_t pod_index,
    float roll_deg,
    float pitch_deg,
    float yaw_relative_deg,
    bool valid
);


/*
 * Update one geophone signal summary.
''',
    '''esp_err_t device_status_set_pod_imu(
    size_t pod_index,
    float roll_deg,
    float pitch_deg,
    float yaw_relative_deg,
    bool valid
);


/*
 * Update temperature/humidity values published by the pod environmental
 * sensor. Capability becomes known as soon as a driver calls this function;
 * individual validity remains false until that value is actually measured.
 */
esp_err_t device_status_set_pod_environment(
    size_t pod_index,
    float temperature_c,
    bool temperature_valid,
    float humidity_percent,
    bool humidity_valid
);


/*
 * Publish a local pod position from a real positioning transport. Calling
 * this with valid=false advertises transport capability without inventing a
 * coordinate.
 */
esp_err_t device_status_set_pod_local_position(
    size_t pod_index,
    float local_x_m,
    float local_y_m,
    bool valid
);


/*
 * Publish resolved global coordinates. Until a real transport/localization
 * layer calls this setter, global_position capability and validity stay false.
 */
esp_err_t device_status_set_pod_global_position(
    size_t pod_index,
    double latitude,
    double longitude,
    double altitude_m,
    bool altitude_valid,
    bool valid
);


/*
 * Update one geophone signal summary.
''',
    'pod environment and position setters',
)
if status_header != status_header_original:
    status_header_path.write_text(status_header, encoding='utf-8')
    print('Expanded device_status pod capability/read model.')

pods_path = Path('components/device_status/device_status_pods.c')
pods = pods_path.read_text(encoding='utf-8')
pods_original = pods
pods = replace_once(
    pods,
    '#include "device_status.h"\n\n#include "device_status_internal.h"',
    '#include "device_status.h"\n\n#include <math.h>\n\n#include "device_status_internal.h"',
    'device status math include',
)
pods = replace_once(
    pods,
    '''    barometer->valid =
        valid;
''',
    '''    status->pods[pod_index].capabilities.barometer =
        true;

    barometer->valid =
        valid;
''',
    'barometer capability publication',
)
pods = replace_once(
    pods,
    '''    imu->valid =
        valid;
''',
    '''    status->pods[pod_index].capabilities.imu =
        true;

    imu->valid =
        valid;
''',
    'imu capability publication',
)
pods = replace_once(
    pods,
    '''esp_err_t device_status_set_pod_geophone(
''',
    '''esp_err_t device_status_set_pod_environment(
    size_t pod_index,
    float temperature_c,
    bool temperature_valid,
    float humidity_percent,
    bool humidity_valid
)
{
    esp_err_t err = device_status_validate_pod(pod_index);
    if (err != ESP_OK) {
        return err;
    }

    if (
        (temperature_valid && !isfinite(temperature_c)) ||
        (humidity_valid && (
            !isfinite(humidity_percent) ||
            humidity_percent < 0.0f ||
            humidity_percent > 100.0f
        ))
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    device_status_snapshot_t *status;
    err = device_status_internal_lock(&status);
    if (err != ESP_OK) {
        return err;
    }

    device_pod_status_t *pod = &status->pods[pod_index];
    pod->capabilities.environment = true;
    pod->environment.valid = temperature_valid || humidity_valid;
    pod->environment.temperature_valid = temperature_valid;
    pod->environment.humidity_valid = humidity_valid;
    pod->environment.temperature_c = temperature_valid ? temperature_c : 0.0f;
    pod->environment.humidity_percent = humidity_valid ? humidity_percent : 0.0f;

    device_status_internal_unlock();
    return ESP_OK;
}


esp_err_t device_status_set_pod_local_position(
    size_t pod_index,
    float local_x_m,
    float local_y_m,
    bool valid
)
{
    esp_err_t err = device_status_validate_pod(pod_index);
    if (err != ESP_OK) {
        return err;
    }
    if (valid && (!isfinite(local_x_m) || !isfinite(local_y_m))) {
        return ESP_ERR_INVALID_ARG;
    }

    device_status_snapshot_t *status;
    err = device_status_internal_lock(&status);
    if (err != ESP_OK) {
        return err;
    }

    device_pod_status_t *pod = &status->pods[pod_index];
    pod->capabilities.local_position = true;
    pod->position.local_valid = valid;
    pod->position.local_x_m = valid ? local_x_m : 0.0f;
    pod->position.local_y_m = valid ? local_y_m : 0.0f;

    device_status_internal_unlock();
    return ESP_OK;
}


esp_err_t device_status_set_pod_global_position(
    size_t pod_index,
    double latitude,
    double longitude,
    double altitude_m,
    bool altitude_valid,
    bool valid
)
{
    esp_err_t err = device_status_validate_pod(pod_index);
    if (err != ESP_OK) {
        return err;
    }
    if (
        valid && (
            !isfinite(latitude) ||
            !isfinite(longitude) ||
            latitude < -90.0 ||
            latitude > 90.0 ||
            longitude < -180.0 ||
            longitude > 180.0 ||
            (altitude_valid && !isfinite(altitude_m))
        )
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    device_status_snapshot_t *status;
    err = device_status_internal_lock(&status);
    if (err != ESP_OK) {
        return err;
    }

    device_pod_status_t *pod = &status->pods[pod_index];
    pod->capabilities.global_position = true;
    pod->position.global_valid = valid;
    pod->position.latitude = valid ? latitude : 0.0;
    pod->position.longitude = valid ? longitude : 0.0;
    pod->position.altitude_valid = valid && altitude_valid;
    pod->position.altitude_m = valid && altitude_valid ? altitude_m : 0.0;

    device_status_internal_unlock();
    return ESP_OK;
}


esp_err_t device_status_set_pod_geophone(
''',
    'environment and position implementations',
)
pods = replace_once(
    pods,
    '''    geophone->valid =
        valid;
''',
    '''    status->pods[pod_index].capabilities.geophone =
        true;

    geophone->valid =
        valid;
''',
    'geophone capability publication',
)
if pods != pods_original:
    pods_path.write_text(pods, encoding='utf-8')
    print('Added pod environment/local/global position publishers.')


# Web JSON exposes capabilities and measured values independently.
json_path = Path('components/web_server/web_status_json_pods.c')
json_text = json_path.read_text(encoding='utf-8')
json_original = json_text
json_text = replace_once(
    json_text,
    '''static bool web_status_json_add_barometer(
''',
    '''static bool web_status_json_add_capabilities(
    cJSON *pod_object,
    const device_pod_capabilities_t *capabilities
)
{
    cJSON *object = cJSON_AddObjectToObject(pod_object, "capabilities");
    if (object == NULL) {
        return false;
    }

    return
        cJSON_AddBoolToObject(object, "barometer", capabilities->barometer) != NULL &&
        cJSON_AddBoolToObject(object, "environment", capabilities->environment) != NULL &&
        cJSON_AddBoolToObject(object, "imu", capabilities->imu) != NULL &&
        cJSON_AddBoolToObject(object, "geophone", capabilities->geophone) != NULL &&
        cJSON_AddBoolToObject(object, "local_position", capabilities->local_position) != NULL &&
        cJSON_AddBoolToObject(object, "global_position", capabilities->global_position) != NULL;
}


static bool web_status_json_add_barometer(
''',
    'pod capabilities JSON',
)
json_text = replace_once(
    json_text,
    '''static bool web_status_json_add_imu(
''',
    '''static bool web_status_json_add_environment(
    cJSON *pod_object,
    const device_pod_environment_status_t *environment
)
{
    cJSON *object = cJSON_AddObjectToObject(pod_object, "environment");
    if (object == NULL) {
        return false;
    }

    return
        cJSON_AddBoolToObject(object, "valid", environment->valid) != NULL &&
        cJSON_AddBoolToObject(object, "temperature_valid", environment->temperature_valid) != NULL &&
        cJSON_AddNumberToObject(object, "temperature_c", environment->temperature_c) != NULL &&
        cJSON_AddBoolToObject(object, "humidity_valid", environment->humidity_valid) != NULL &&
        cJSON_AddNumberToObject(object, "humidity_percent", environment->humidity_percent) != NULL;
}


static bool web_status_json_add_imu(
''',
    'pod environment JSON',
)
json_text = replace_once(
    json_text,
    '''bool web_status_json_add_pods(
''',
    '''static bool web_status_json_add_position(
    cJSON *pod_object,
    const device_pod_position_status_t *position
)
{
    cJSON *object = cJSON_AddObjectToObject(pod_object, "position");
    if (object == NULL) {
        return false;
    }

    return
        cJSON_AddBoolToObject(object, "local_valid", position->local_valid) != NULL &&
        cJSON_AddNumberToObject(object, "local_x_m", position->local_x_m) != NULL &&
        cJSON_AddNumberToObject(object, "local_y_m", position->local_y_m) != NULL &&
        cJSON_AddBoolToObject(object, "global_valid", position->global_valid) != NULL &&
        cJSON_AddNumberToObject(object, "latitude", position->latitude) != NULL &&
        cJSON_AddNumberToObject(object, "longitude", position->longitude) != NULL &&
        cJSON_AddBoolToObject(object, "altitude_valid", position->altitude_valid) != NULL &&
        cJSON_AddNumberToObject(object, "altitude_m", position->altitude_m) != NULL;
}


bool web_status_json_add_pods(
''',
    'pod position JSON',
)
json_text = replace_once(
    json_text,
    '''        if (
            !web_status_json_add_barometer(
                pod_object,
                &pod->barometer
            )
        ) {
            return false;
        }

        if (
            !web_status_json_add_imu(
''',
    '''        if (
            !web_status_json_add_capabilities(
                pod_object,
                &pod->capabilities
            )
        ) {
            return false;
        }

        if (
            !web_status_json_add_barometer(
                pod_object,
                &pod->barometer
            )
        ) {
            return false;
        }

        if (
            !web_status_json_add_environment(
                pod_object,
                &pod->environment
            )
        ) {
            return false;
        }

        if (
            !web_status_json_add_imu(
''',
    'pod JSON capability/environment wiring',
)
json_text = replace_once(
    json_text,
    '''        if (
            !web_status_json_add_geophone(
                pod_object,
                &pod->geophone
            )
        ) {
            return false;
        }
''',
    '''        if (
            !web_status_json_add_geophone(
                pod_object,
                &pod->geophone
            )
        ) {
            return false;
        }

        if (
            !web_status_json_add_position(
                pod_object,
                &pod->position
            )
        ) {
            return false;
        }
''',
    'pod JSON position wiring',
)
if json_text != json_original:
    json_path.write_text(json_text, encoding='utf-8')
    print('Exposed pod capabilities/environment/position in status JSON.')


# Browser read model mirrors firmware validity exactly.
domain_path = Path('webui/src/domain/device.ts')
domain = domain_path.read_text(encoding='utf-8')
domain_original = domain
domain = replace_once(
    domain,
    '''export interface PodImu extends ValidValue {
  roll_deg: number;
  pitch_deg: number;
  yaw_relative_deg: number;
}

export interface PodGeophone extends ValidValue {
''',
    '''export interface PodImu extends ValidValue {
  roll_deg: number;
  pitch_deg: number;
  yaw_relative_deg: number;
}

export interface PodEnvironment extends ValidValue {
  temperature_valid: boolean;
  temperature_c: number;
  humidity_valid: boolean;
  humidity_percent: number;
}

export interface PodPosition {
  local_valid: boolean;
  local_x_m: number;
  local_y_m: number;
  global_valid: boolean;
  latitude: number;
  longitude: number;
  altitude_valid: boolean;
  altitude_m: number;
}

export interface PodCapabilities {
  barometer: boolean;
  environment: boolean;
  imu: boolean;
  geophone: boolean;
  local_position: boolean;
  global_position: boolean;
}

export interface PodGeophone extends ValidValue {
''',
    'browser pod support types',
)
domain = replace_once(
    domain,
    '''export interface DevicePod {
  id: number;
  state: string;
  barometer: PodBarometer;
  imu: PodImu;
  geophone: PodGeophone;
}
''',
    '''export interface DevicePod {
  id: number;
  state: string;
  capabilities: PodCapabilities;
  barometer: PodBarometer;
  environment: PodEnvironment;
  imu: PodImu;
  geophone: PodGeophone;
  position: PodPosition;
}
''',
    'browser pod aggregate model',
)
if domain != domain_original:
    domain_path.write_text(domain, encoding='utf-8')
    print('Updated browser pod status types.')

sensors_path = Path('webui/src/features/sensors/SensorsFeature.tsx')
sensors = sensors_path.read_text(encoding='utf-8')
sensors_original = sensors
sensors = replace_once(
    sensors,
    '''                <div><dt>Temperature</dt><dd>awaiting pod environmental driver</dd></div>
''',
    '''                <div><dt>Temperature</dt><dd>{pod ? measured(pod.environment.temperature_valid, `${pod.environment.temperature_c.toFixed(1)} °C`) : 'unavailable'}</dd></div>
                <div><dt>Humidity</dt><dd>{pod ? measured(pod.environment.humidity_valid, `${pod.environment.humidity_percent.toFixed(1)} %`) : 'unavailable'}</dd></div>
''',
    'measured pod environment UI',
)
sensors = replace_once(
    sensors,
    '''                <div><dt>Local X/Y</dt><dd>awaiting XIAO nRF54L15</dd></div>
                <div><dt>Global coordinates</dt><dd>{object?.location.valid ? 'base GPS bound; waiting for local offset' : 'base GPS not bound'}</dd></div>
''',
    '''                <div><dt>Local X/Y</dt><dd>{pod ? measured(pod.position.local_valid, `${pod.position.local_x_m.toFixed(2)} / ${pod.position.local_y_m.toFixed(2)} m`) : 'unavailable'}</dd></div>
                <div><dt>Global coordinates</dt><dd>{pod?.position.global_valid
                  ? `${pod.position.latitude.toFixed(6)}, ${pod.position.longitude.toFixed(6)}${pod.position.altitude_valid ? ` / ${pod.position.altitude_m.toFixed(1)} m` : ''}`
                  : object?.location.valid
                    ? 'base GPS bound; pod position unavailable'
                    : 'unavailable'}</dd></div>
''',
    'measured pod positioning UI',
)
if sensors != sensors_original:
    sensors_path.write_text(sensors, encoding='utf-8')
    print('Updated sensor diagnostics to render only published measurements.')

print('Next device console optimization patch complete.')
