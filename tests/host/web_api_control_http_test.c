#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "seismic_acquisition.h"

#include "cJSON.h"
#include "esp_http_server.h"
#include "freertos/semphr.h"
#include "localization_calculate.h"
#include "measurement_service.h"
#include "nrf_topology_service.h"
#include "object_store.h"
#include "storage_bulk_admission.h"
#include "storage_manager_internal.h"
#include "web_server_internal.h"
#include "workflow_service.h"

typedef struct {
    bool locked;
} fake_admission_mutex_t;

static object_store_location_t s_saved_location;
static unsigned s_location_calls;
static unsigned s_cache_invalidations;
static unsigned s_cache_refresh_requests;
static char s_previous_active_id[OBJECT_STORE_ID_SIZE];
static esp_err_t s_set_active_result;
static char s_last_set_active_id[OBJECT_STORE_ID_SIZE];
static unsigned s_set_active_calls;
static unsigned s_clear_staging_calls;
static char s_cleared_staging_id[OBJECT_STORE_ID_SIZE];
static unsigned s_calculate_calls;
static localization_calculate_config_t s_last_calculate_config;
static char s_last_calculate_session_id[32];
static esp_err_t s_calculate_result;
static uint32_t s_last_budget_ms;
static unsigned s_calc_settings_save_calls;
static char s_saved_calc_settings[2048];
static unsigned s_calc_settings_notify_calls;
static esp_err_t s_acquisition_start_result;
static unsigned s_acquisition_start_calls;
static esp_err_t s_measurement_start_result;
static unsigned s_measurement_start_calls;
static unsigned s_lease_acquire_calls;
static unsigned s_lease_release_calls;
static unsigned s_scan_store_calls;
static int s_measurement_state;
static esp_err_t s_height_capture_result;
static uint8_t s_height_capture_channel_mask;
static uint16_t s_height_capture_generation;
static unsigned s_height_capture_calls;
static unsigned s_sensor_scan_calls;
static uint8_t s_sensor_scan_channel;
static unsigned s_height_capture_wire_sends;
static uint16_t s_main_capture_generation;
static unsigned s_main_capture_calls;
static fake_admission_mutex_t *s_admission_mutex;
static workflow_command_result_t s_workflow_result;
static workflow_command_t s_workflow_command;
static workflow_command_params_t s_workflow_params;
static char s_workflow_reason[WORKFLOW_REASON_MAX];
static unsigned s_workflow_command_calls;
static unsigned s_workflow_snapshot_calls;
static workflow_snapshot_t s_workflow_snapshot;

static void reset_fakes(void)
{
    s_sensor_scan_calls = 0U;
    s_sensor_scan_channel = 0U;
    memset(&s_saved_location, 0, sizeof(s_saved_location));
    s_location_calls = 0U;
    s_cache_invalidations = 0U;
    s_cache_refresh_requests = 0U;
    (void)snprintf(s_previous_active_id, sizeof(s_previous_active_id), "%s", "OBJ_0001");
    s_set_active_result = ESP_OK;
    s_last_set_active_id[0] = '\0';
    s_set_active_calls = 0U;
    s_clear_staging_calls = 0U;
    s_cleared_staging_id[0] = '\0';
    s_calculate_calls = 0U;
    memset(&s_last_calculate_config, 0, sizeof(s_last_calculate_config));
    memset(s_last_calculate_session_id, 0, sizeof(s_last_calculate_session_id));
    s_calculate_result = ESP_OK;
    s_last_budget_ms = 12345U;
    s_calc_settings_save_calls = 0U;
    s_saved_calc_settings[0] = '\0';
    s_calc_settings_notify_calls = 0U;
    s_acquisition_start_result = ESP_OK;
    s_acquisition_start_calls = 0U;
    s_measurement_start_result = ESP_OK;
    s_measurement_start_calls = 0U;
    s_lease_acquire_calls = 0U;
    s_lease_release_calls = 0U;
    s_scan_store_calls = 0U;
    s_measurement_state = MEASUREMENT_SERVICE_LIVE;
    s_height_capture_result = ESP_OK;
    s_height_capture_channel_mask = 0U;
    s_height_capture_generation = 42U;
    s_height_capture_calls = 0U;
    s_height_capture_wire_sends = 0U;
    s_main_capture_generation = 0U;
    s_main_capture_calls = 0U;
    s_workflow_result = WORKFLOW_COMMAND_RESULT_OK;
    s_workflow_command = WORKFLOW_COMMAND_COUNT;
    memset(&s_workflow_params, 0, sizeof(s_workflow_params));
    s_workflow_reason[0] = '\0';
    s_workflow_command_calls = 0U;
    s_workflow_snapshot_calls = 0U;
    s_workflow_snapshot = (workflow_snapshot_t){
        .step = WORKFLOW_STEP_OBJECT,
        .generation = 1U,
        .sensor_sufficiency = WORKFLOW_SENSOR_INSUFFICIENT,
        .reason = "",
        .progress = WORKFLOW_PROGRESS_UNKNOWN,
        .recommended_action = WORKFLOW_COMMAND_COUNT,
    };
    storage_bulk_admission_exit_protected();
}


SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    s_admission_mutex = calloc(1U, sizeof(*s_admission_mutex));
    return s_admission_mutex;
}


BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout)
{
    (void)timeout;
    fake_admission_mutex_t *mutex = semaphore;
    assert(mutex != NULL && mutex == s_admission_mutex);
    if (mutex->locked) return pdFALSE;
    mutex->locked = true;
    return pdTRUE;
}


BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    fake_admission_mutex_t *mutex = semaphore;
    assert(mutex != NULL && mutex == s_admission_mutex && mutex->locked);
    mutex->locked = false;
    return pdTRUE;
}

bool power_control_is_shutdown_requested(void)
{
    return false;
}

bool measurement_service_snapshot_allows_object_mutation(
    const measurement_service_snapshot_t *snapshot
)
{
    return snapshot != NULL && !snapshot->busy;
}

bool measurement_service_snapshot_blocks_acquisition_stop(
    const measurement_service_snapshot_t *snapshot
)
{
    return snapshot != NULL && snapshot->busy;
}

esp_err_t measurement_service_get_snapshot(measurement_service_snapshot_t *snapshot)
{
    if (snapshot == NULL) return ESP_ERR_INVALID_ARG;
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->state = s_measurement_state;
    return ESP_OK;
}

esp_err_t nrf_topology_service_request_height_reference_capture(
    uint8_t *requested_channel_mask,
    uint16_t *requested_generation)
{
    assert(requested_channel_mask != NULL);
    assert(requested_generation != NULL);
    ++s_height_capture_calls;
    if (s_height_capture_result == ESP_OK) {
        *requested_channel_mask = s_height_capture_channel_mask;
        *requested_generation = s_height_capture_generation;
    }
    return s_height_capture_result;
}

esp_err_t nrf_topology_service_request_sensor_scan(uint8_t channel, uint32_t *request_id)
{
    ++s_sensor_scan_calls;
    s_sensor_scan_channel = channel;
    *request_id = 1234U;
    return ESP_OK;
}

esp_err_t nrf_topology_service_get_sensor_scan_dispatch(uint32_t request_id, bool *pending, esp_err_t *result)
{
    if (request_id != 1234U) return ESP_ERR_NOT_FOUND;
    *pending = false;
    *result = ESP_FAIL;
    return ESP_OK;
}

esp_err_t main_barometer_request_height_reference_capture(uint16_t generation)
{
    s_main_capture_calls++;
    s_main_capture_generation = generation;
    return generation != 0U ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t workflow_service_capture_height_references(
    uint8_t *requested_channel_mask,
    uint16_t *requested_generation)
{
    const esp_err_t err = nrf_topology_service_request_height_reference_capture(
        requested_channel_mask, requested_generation);
    if (err == ESP_OK) {
        (void)main_barometer_request_height_reference_capture(*requested_generation);
    }
    return err;
}

esp_err_t workflow_service_notify_calc_settings_saved(const char *object_id)
{
    assert(object_id != NULL);
    ++s_calc_settings_notify_calls;
    return ESP_OK;
}

esp_err_t measurement_service_start_object_async(const char *object_id)
{
    (void)object_id;
    s_measurement_start_calls++;
    return s_measurement_start_result;
}

esp_err_t measurement_service_stop_async(void)
{
    return ESP_OK;
}

esp_err_t seismic_acquisition_start(void)
{
    s_acquisition_start_calls++;
    return s_acquisition_start_result;
}

esp_err_t seismic_acquisition_set_preview(seismic_preview_t preview, bool active)
{
    assert(preview == SEISMIC_PREVIEW_WEB);
    if (!active) return ESP_OK;
    return seismic_acquisition_start();
}

void seismic_acquisition_stop(void)
{
}

bool seismic_recorder_is_recording_object(const char *object_id)
{
    (void)object_id;
    return false;
}

esp_err_t object_store_create_named(const char *name, object_store_summary_t *created_object)
{
    (void)name;
    (void)created_object;
    return ESP_OK;
}

esp_err_t object_store_get_active(object_store_summary_t *object)
{
    assert(object != NULL);
    memset(object, 0, sizeof(*object));
    object->active = true;
    (void)snprintf(object->id, sizeof(object->id), "%s", s_previous_active_id);
    return ESP_OK;
}

esp_err_t object_store_set_active(const char *object_id)
{
    assert(object_id != NULL);
    ++s_set_active_calls;
    (void)snprintf(s_last_set_active_id, sizeof(s_last_set_active_id), "%s", object_id);
    return s_set_active_result;
}

esp_err_t web_scan_store_clear_staging(const char *object_id)
{
    assert(object_id != NULL);
    assert(s_set_active_calls == 1U && s_set_active_result == ESP_OK);
    ++s_clear_staging_calls;
    (void)snprintf(s_cleared_staging_id, sizeof(s_cleared_staging_id), "%s", object_id);
    return ESP_OK;
}

esp_err_t object_store_delete(const char *object_id)
{
    (void)object_id;
    return ESP_OK;
}

esp_err_t object_store_rename(const char *object_id, const char *name)
{
    (void)object_id;
    (void)name;
    return ESP_OK;
}

esp_err_t object_store_set_location(
    const char *object_id,
    const object_store_location_t *location
)
{
    assert(object_id != NULL && location != NULL);
    s_saved_location = *location;
    s_location_calls++;
    return ESP_OK;
}

esp_err_t object_store_set_calc_settings_json(const char *object_id, const char *json)
{
    assert(object_id != NULL && json != NULL);
    ++s_calc_settings_save_calls;
    (void)snprintf(s_saved_calc_settings, sizeof(s_saved_calc_settings), "%s", json);
    return ESP_OK;
}

esp_err_t object_store_set_cable_length(const char *object_id, size_t pod_index, float length_m)
{
    (void)object_id;
    (void)pod_index;
    (void)length_m;
    return ESP_OK;
}

esp_err_t object_store_set_cable_default(size_t pod_index, float length_m)
{
    (void)pod_index;
    (void)length_m;
    return ESP_OK;
}

esp_err_t object_store_lease_acquire(const char *object_id, object_store_lease_t *lease)
{
    (void)object_id;
    ++s_lease_acquire_calls;
    if (lease != NULL) lease->token_id = 1U;
    return ESP_OK;
}

esp_err_t object_store_lease_release(object_store_lease_t *lease)
{
    (void)lease;
    ++s_lease_release_calls;
    return ESP_OK;
}

int64_t esp_timer_get_time(void)
{
    return 1000000;
}

/* calculate_source hands the run to the background job; the stub records
 * what was submitted (the job itself is tested in localization_job). */
esp_err_t localization_job_submit(
    const char *object_id,
    const char *session_id,
    const localization_calculate_config_t *config,
    uint32_t budget_ms,
    uint32_t *job_id_out
)
{
    (void)object_id;
    (void)job_id_out;
    assert(config != NULL);
    s_calculate_calls++;
    s_last_calculate_config = *config;
    s_last_budget_ms = budget_ms;
    snprintf(s_last_calculate_session_id, sizeof(s_last_calculate_session_id), "%s", session_id);
    return s_calculate_result;
}

bool object_format_id_is_valid(const char *object_id)
{
    return object_id != NULL && strcmp(object_id, "OBJ_0001") == 0;
}

esp_err_t web_scan_store_try_handle(httpd_req_t *request, bool *handled)
{
    (void)request;
    ++s_scan_store_calls;
    if (handled != NULL) *handled = request != NULL && request->query != NULL;
    return ESP_OK;
}

void web_status_json_objects_invalidate(void)
{
    s_cache_invalidations++;
}

esp_err_t web_status_json_objects_request_refresh(void)
{
    s_cache_refresh_requests++;
    return ESP_OK;
}

void web_status_json_objects_set_active(const char *object_id)
{
    (void)object_id;
}

void web_server_route_tracking_begin(size_t capacity)
{
    (void)capacity;
}

esp_err_t web_server_route_tracking_finish(const web_route_identity_t *catalog, size_t count)
{
    (void)catalog;
    (void)count;
    return ESP_OK;
}

esp_err_t web_server_route_tracking_register(const char *uri, int method)
{
    (void)uri;
    (void)method;
    return ESP_OK;
}

size_t httpd_req_get_url_query_len(httpd_req_t *request)
{
    return request != NULL && request->query != NULL ? strlen(request->query) : 0U;
}

esp_err_t httpd_req_get_url_query_str(httpd_req_t *request, char *buffer, size_t buffer_size)
{
    if (request == NULL || request->query == NULL || buffer == NULL || buffer_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t length = strlen(request->query);
    if (length >= buffer_size) return ESP_ERR_INVALID_SIZE;
    memcpy(buffer, request->query, length + 1U);
    return ESP_OK;
}

esp_err_t httpd_query_key_value(const char *query, const char *key, char *value, size_t value_size)
{
    if (query == NULL || key == NULL || value == NULL || value_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t key_length = strlen(key);
    const char *cursor = query;
    while (*cursor != '\0') {
        const char *end = strchr(cursor, '&');
        const char *equals = strchr(cursor, '=');
        if (equals != NULL && (end == NULL || equals < end)) {
            if ((size_t)(equals - cursor) == key_length && strncmp(cursor, key, key_length) == 0) {
                const char *start = equals + 1;
                const size_t length = end == NULL ? strlen(start) : (size_t)(end - start);
                if (length == 0U || length >= value_size) return ESP_ERR_INVALID_SIZE;
                memcpy(value, start, length);
                value[length] = '\0';
                return ESP_OK;
            }
        }
        if (end == NULL) break;
        cursor = end + 1;
    }
    return ESP_ERR_NOT_FOUND;
}

int httpd_req_recv(httpd_req_t *request, void *buffer, size_t length)
{
    if (request == NULL || buffer == NULL || request->body == NULL) return ESP_FAIL;
    const size_t total = (size_t)request->content_len;
    if (request->body_offset >= total) return 0;
    const size_t remaining = total - request->body_offset;
    const size_t copied = remaining < length ? remaining : length;
    memcpy(buffer, request->body + request->body_offset, copied);
    request->body_offset += copied;
    return (int)copied;
}

esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type)
{
    request->content_type = type;
    return ESP_OK;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *field, const char *value)
{
    if (strcmp(field, "Cache-Control") == 0) request->cache_control = value;
    return ESP_OK;
}

esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status)
{
    request->response_status = status;
    return ESP_OK;
}

esp_err_t httpd_resp_send(httpd_req_t *request, const char *buffer, ssize_t length)
{
    assert(request != NULL && buffer != NULL);
    const size_t bytes = length == HTTPD_RESP_USE_STRLEN ? strlen(buffer) : (size_t)length;
    assert(bytes + 1U <= request->response_capacity);
    memcpy(request->response_body, buffer, bytes);
    request->response_body[bytes] = '\0';
    request->response_length = bytes;
    return ESP_OK;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *uri)
{
    assert(server != NULL && uri != NULL);
    return ESP_OK;
}

/* The production file is included so the test invokes its real handler and
 * JSON/body/read/admission logic, not a duplicate test implementation. */
#define static
#include "../../components/web_server/web_api_control.c"
#undef static
#include "../../components/workflow_service/workflow_core.c"

workflow_command_result_t workflow_service_command(
    workflow_command_t command,
    const workflow_command_params_t *params
)
{
    assert(params != NULL);
    ++s_workflow_command_calls;
    s_workflow_command = command;
    s_workflow_params = *params;
    s_workflow_params.reason = NULL;
    if (params->reason != NULL) {
        (void)snprintf(s_workflow_reason, sizeof(s_workflow_reason), "%s", params->reason);
    }
    return s_workflow_result;
}

esp_err_t workflow_service_get_snapshot(workflow_snapshot_t *snapshot)
{
    assert(snapshot != NULL);
    ++s_workflow_snapshot_calls;
    *snapshot = s_workflow_snapshot;
    return ESP_OK;
}

bool web_status_json_add_workflow(cJSON *root)
{
    workflow_snapshot_t snapshot;
    if (workflow_service_get_snapshot(&snapshot) != ESP_OK) return false;
    cJSON *workflow = cJSON_AddObjectToObject(root, "workflow");
    return workflow != NULL &&
        cJSON_AddStringToObject(workflow, "step", workflow_core_step_name(snapshot.step)) != NULL &&
        cJSON_AddNumberToObject(workflow, "generation", snapshot.generation) != NULL;
}

static httpd_req_t make_request(const char *body, char *response, size_t response_capacity)
{
    return (httpd_req_t){
        .uri = "/api/control",
        .body = body,
        .content_len = (int)strlen(body),
        .response_body = response,
        .response_capacity = response_capacity,
    };
}


static httpd_req_t make_scan_request(
    const char *query,
    char *response,
    size_t response_capacity
)
{
    return (httpd_req_t){
        .uri = "/api/control",
        .query = query,
        .content_len = 0,
        .response_body = response,
        .response_capacity = response_capacity,
    };
}


static void test_scan_recording_gate_precedes_object_lease(void)
{
    reset_fakes();
    assert(storage_bulk_admission_enter_protected() == ESP_OK);

    const char *queries[] = {
        "scan=save&object_id=OBJ_0001&session_id=S000001",
        "scan=load&kind=occupancy&object_id=OBJ_0001&session_id=S000001",
    };
    for (size_t index = 0U; index < sizeof(queries) / sizeof(queries[0]); ++index) {
        char response[256] = {0};
        httpd_req_t request = make_scan_request(queries[index], response, sizeof(response));
        assert(web_api_control_handler(&request) == ESP_OK);
        assert(s_scan_store_calls == index + 1U);
        assert(s_lease_acquire_calls == 0U);
        assert(s_lease_release_calls == 0U);
    }

    /* Registration remains on its existing lease path during global admission. */
    {
        char response[256] = {0};
        httpd_req_t request = make_scan_request(
            "scan=save&kind=registration&object_id=OBJ_0001&session_id=S000001",
            response,
            sizeof(response)
        );
        assert(web_api_control_handler(&request) == ESP_OK);
        assert(s_lease_acquire_calls == 1U);
        assert(s_lease_release_calls == 1U);
    }

    storage_bulk_admission_exit_protected();
}

static void test_measurement_start_requires_workflow_step(void)
{
    static const struct {
        workflow_step_t step;
        bool allowed;
    } cases[] = {
        {WORKFLOW_STEP_OBJECT, false},
        {WORKFLOW_STEP_SENSOR_CHECK, false},
        {WORKFLOW_STEP_RECORD_READY, true},
        {WORKFLOW_STEP_RECORDING, true},
    };
    static const char *const body =
        "{\"action\":\"measurement_start\",\"object_id\":\"OBJ_0001\"}";

    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        reset_fakes();
        s_workflow_snapshot.step = cases[i].step;
        char response[512] = {0};
        httpd_req_t request = make_request(body, response, sizeof(response));

        assert(web_api_control_handler(&request) == ESP_OK);
        assert(s_workflow_snapshot_calls == 1U);
        assert(strstr(response, "\"action\":\"measurement_start\"") != NULL);
        if (cases[i].allowed) {
            assert(request.response_status == NULL);
            assert(strstr(response, "\"ok\":true") != NULL);
            assert(s_acquisition_start_calls == 1U);
            assert(s_measurement_start_calls == 1U);
        }
        else {
            assert(request.response_status != NULL);
            assert(strcmp(request.response_status, "409 Conflict") == 0);
            assert(strstr(response, "\"ok\":false") != NULL);
            assert(strstr(response, "\"error_code\":\"workflow_step_not_allowed\"") != NULL);
            assert(strstr(response, "\"message\":\"measurement_start is only allowed in RECORD_READY or RECORDING\"") != NULL);
            assert(s_acquisition_start_calls == 0U);
            assert(s_measurement_start_calls == 0U);
        }
    }
}

static void test_measurement_start_rejects_acquisition_failures(void)
{
    static const struct {
        esp_err_t result;
        const char *error_name;
    } failures[] = {
        {ESP_ERR_INVALID_STATE, "ESP_ERR_INVALID_STATE"},
        {ESP_ERR_TIMEOUT, "ESP_ERR_TIMEOUT"},
        {ESP_ERR_NOT_FOUND, "ESP_ERR_NOT_FOUND"},
    };
    static const char *const body =
        "{\"action\":\"measurement_start\",\"object_id\":\"OBJ_0001\"}";

    for (size_t i = 0U; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        reset_fakes();
        s_workflow_snapshot.step = WORKFLOW_STEP_RECORD_READY;
        s_acquisition_start_result = failures[i].result;
        char response[512] = {0};
        httpd_req_t request = make_request(body, response, sizeof(response));

        assert(web_api_control_handler(&request) == ESP_OK);
        assert(request.response_status != NULL);
        assert(strcmp(request.response_status, "503 Service Unavailable") == 0);
        assert(strstr(response, "\"ok\":false") != NULL);
        assert(strstr(response, "\"action\":\"measurement_start\"") != NULL);
        assert(strstr(response, "\"error_class\":\"acquisition_unavailable\"") != NULL);
        char error_field[96];
        (void)snprintf(error_field, sizeof(error_field), "\"error\":\"%s\"", failures[i].error_name);
        assert(strstr(response, error_field) != NULL);
        assert(s_acquisition_start_calls == 1U);
        assert(s_measurement_start_calls == 0U);
    }
}

static void test_measurement_start_success_stays_async(void)
{
    reset_fakes();
    s_workflow_snapshot.step = WORKFLOW_STEP_RECORD_READY;
    static const char *const body =
        "{\"action\":\"measurement_start\",\"object_id\":\"OBJ_0001\"}";
    char response[512] = {0};
    httpd_req_t request = make_request(body, response, sizeof(response));

    assert(web_api_control_handler(&request) == ESP_OK);
    assert(request.response_status == NULL);
    assert(strstr(response, "\"ok\":true") != NULL);
    assert(strstr(response, "\"action\":\"measurement_start\"") != NULL);
    assert(s_acquisition_start_calls == 1U);
    assert(s_measurement_start_calls == 1U);
}

static void test_measurement_start_async_failure_keeps_existing_mapping(void)
{
    reset_fakes();
    s_workflow_snapshot.step = WORKFLOW_STEP_RECORD_READY;
    s_measurement_start_result = ESP_ERR_INVALID_STATE;
    static const char *const body =
        "{\"action\":\"measurement_start\",\"object_id\":\"OBJ_0001\"}";
    char response[512] = {0};
    httpd_req_t request = make_request(body, response, sizeof(response));

    assert(web_api_control_handler(&request) == ESP_OK);
    assert(request.response_status != NULL);
    assert(strcmp(request.response_status, "409 Conflict") == 0);
    assert(strstr(response, "\"ok\":false") != NULL);
    assert(strstr(response, "\"error\":\"ESP_ERR_INVALID_STATE\"") != NULL);
    assert(strstr(response, "error_class") == NULL);
    assert(s_acquisition_start_calls == 1U);
    assert(s_measurement_start_calls == 1U);
}

static void test_calibrate_heights_rejects_active_measurements(void)
{
    static const int active_states[] = {
        MEASUREMENT_SERVICE_STARTING,
        MEASUREMENT_SERVICE_RECORDING,
        MEASUREMENT_SERVICE_STOPPING,
    };
    static const char *const body = "{\"action\":\"calibrate_heights\"}";

    for (size_t index = 0U; index < sizeof(active_states) / sizeof(active_states[0]); ++index) {
        reset_fakes();
        s_measurement_state = active_states[index];
        char response[256] = {0};
        httpd_req_t request = make_request(body, response, sizeof(response));

        assert(web_api_control_handler(&request) == ESP_OK);
        assert(strcmp(request.response_status, "409 Conflict") == 0);
        assert(strcmp(response, "{\"ok\":false,\"busy_reason\":\"recording_active\"}") == 0);
        assert(s_height_capture_calls == 0U);
        assert(s_height_capture_wire_sends == 0U);
        assert(s_main_capture_calls == 0U);
    }
}

static void test_calibrate_heights_rejects_no_remotes(void)
{
    reset_fakes();
    s_height_capture_result = ESP_ERR_NOT_FOUND;
    char response[256] = {0};
    httpd_req_t request = make_request("{\"action\":\"calibrate_heights\"}", response, sizeof(response));

    assert(web_api_control_handler(&request) == ESP_OK);
    assert(strcmp(request.response_status, "409 Conflict") == 0);
    assert(strcmp(response, "{\"ok\":false,\"error\":\"no_remotes\"}") == 0);
    assert(s_height_capture_calls == 1U);
    assert(s_height_capture_wire_sends == 0U);
    assert(s_main_capture_calls == 0U);
}

static void test_calibrate_heights_queues_channel_mask_without_bus_io(void)
{
    reset_fakes();
    s_height_capture_channel_mask = (uint8_t)((1U << 0U) | (1U << 1U) | (1U << 3U));
    char response[256] = {0};
    httpd_req_t request = make_request("{\"action\":\"calibrate_heights\"}", response, sizeof(response));

    assert(web_api_control_handler(&request) == ESP_OK);
    assert(request.response_status == NULL);
    assert(strcmp(
        response,
        "{\"ok\":true,\"action\":\"calibrate_heights\",\"requested_channels\":[0,1,3]}") == 0);
    assert(s_height_capture_calls == 1U);
    assert(s_height_capture_wire_sends == 0U);
    assert(s_main_capture_calls == 1U);
    assert(s_main_capture_generation == s_height_capture_generation);
}

static void test_phone_location_handler_round_trip(void)
{
    reset_fakes();
    const char *body =
        "{\"action\":\"set_object_location\",\"object_id\":\"OBJ_0001\","
        "\"latitude\":50.4501,\"longitude\":30.5234,\"accuracy_m\":3.2,"
        "\"altitude_m\":179.4,\"altitude_accuracy_m\":5.5,"
        "\"captured_at_ms\":123456789}";
    char response[512] = {0};
    httpd_req_t request = make_request(body, response, sizeof(response));

    assert(web_api_control_handler(&request) == ESP_OK);
    assert(request.response_status == NULL);
    assert(strstr(response, "\"ok\":true") != NULL);
    assert(strstr(response, "set_object_location") != NULL);
    assert(s_location_calls == 1U);
    assert(s_saved_location.valid);
    assert(s_saved_location.latitude == 50.4501);
    assert(s_saved_location.longitude == 30.5234);
    assert(s_saved_location.accuracy_m == 3.2f);
    assert(s_saved_location.altitude_valid);
    assert(s_saved_location.altitude_m == 179.4);
    assert(strcmp(s_saved_location.source, "PHONE_GPS") == 0);
    assert(s_cache_invalidations == 1U);
    assert(s_cache_refresh_requests == 1U);
}

static void test_phone_location_handler_rejects_negative_capture_time(void)
{
    reset_fakes();
    const char *body =
        "{\"action\":\"set_object_location\",\"object_id\":\"OBJ_0001\","
        "\"latitude\":50.4501,\"longitude\":30.5234,\"accuracy_m\":3.2,"
        "\"captured_at_ms\":-1}";
    char response[512] = {0};
    httpd_req_t request = make_request(body, response, sizeof(response));

    assert(web_api_control_handler(&request) == ESP_OK);
    assert(request.response_status != NULL);
    assert(strcmp(request.response_status, "400 Bad Request") == 0);
    assert(strstr(response, "\"ok\":false") != NULL);
    assert(s_location_calls == 0U);
}

static void test_phone_location_handler_rejects_unrepresentable_capture_time(void)
{
    const char *const bad[] = { "1e30", "1.5", "1e999", "18446744073709551616" };
    for (size_t index = 0U; index < sizeof(bad) / sizeof(bad[0]); ++index) {
        reset_fakes();
        char body[512];
        (void)snprintf(
            body, sizeof(body),
            "{\"action\":\"set_object_location\",\"object_id\":\"OBJ_0001\","
            "\"latitude\":50.4501,\"longitude\":30.5234,\"accuracy_m\":3.2,"
            "\"captured_at_ms\":%s}",
            bad[index]
        );
        char response[512] = {0};
        httpd_req_t request = make_request(body, response, sizeof(response));

        assert(web_api_control_handler(&request) == ESP_OK);
        assert(request.response_status != NULL);
        assert(strcmp(request.response_status, "400 Bad Request") == 0);
        assert(strstr(response, "\"ok\":false") != NULL);
        assert(s_location_calls == 0U);
    }
}

extern int cJSON_stub_fail_alloc_countdown;

/*
 * Every allocation inside the control handler is made to fail once, in turn.
 * Whatever happens, a reply that is actually sent must be a complete contract
 * object (ok + action, plus error when ok is false); otherwise nothing is sent.
 */
static void test_control_response_is_complete_or_not_sent_under_allocation_failure(void)
{
    const char *body =
        "{\"action\":\"set_object_location\",\"object_id\":\"OBJ_0001\","
        "\"latitude\":50.4501,\"longitude\":30.5234,\"accuracy_m\":3.2,"
        "\"captured_at_ms\":-1}";
    unsigned sent = 0U;
    unsigned suppressed = 0U;
    bool swept_every_allocation = false;

    for (int failing_allocation = 0; failing_allocation < 200; ++failing_allocation) {
        reset_fakes();
        char response[512] = {0};
        httpd_req_t request = make_request(body, response, sizeof(response));

        cJSON_stub_fail_alloc_countdown = failing_allocation;
        (void)web_api_control_handler(&request);
        const bool failure_was_injected = cJSON_stub_fail_alloc_countdown == -1;
        cJSON_stub_fail_alloc_countdown = -1;

        if (response[0] != '\0') {
            cJSON *parsed = cJSON_Parse(response);
            assert(parsed != NULL);
            assert(cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(parsed, "ok")));
            assert(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(parsed, "action")));
            if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(parsed, "ok"))) {
                assert(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(parsed, "error")));
            }
            cJSON_Delete(parsed);
            ++sent;
        } else {
            ++suppressed;
        }
        if (!failure_was_injected) {
            swept_every_allocation = true;
            break;
        }
    }
    assert(swept_every_allocation);
    assert(sent > 0U);
    assert(suppressed > 0U);
}

static const char *const CALCULATE_SOURCE_BODY =
    "{\"action\":\"calculate_source\",\"object_id\":\"OBJ_0001\",\"session_id\":\"S000001\","
    "\"propagation_speed_mps\":340.0,\"propagation_speed_uncertainty_mps\":5.0,"
    "\"propagation_model_name\":\"tap_calibrated\",\"timing_uncertainty_us\":2000.0,"
    "\"noise_window_samples\":50,\"sta_window_samples\":5,\"lta_window_samples\":50,"
    "\"sta_lta_trigger_ratio\":3.5,\"refine_lookback_samples\":10,\"refine_lookahead_samples\":10,"
    "\"refine_amplitude_fraction\":0.5,\"snr_signal_window_samples\":50,\"minimum_snr\":2.0,"
    "\"semblance_window_half_span_samples\":25}";

static void test_calculate_source_action_submits_a_background_job(void)
{
    reset_fakes();
    s_calculate_result = ESP_OK;
    char response[512] = {0};
    httpd_req_t request = make_request(CALCULATE_SOURCE_BODY, response, sizeof(response));

    assert(web_api_control_handler(&request) == ESP_OK);
    assert(request.response_status == NULL);
    assert(strstr(response, "\"ok\":true") != NULL);
    assert(strstr(response, "calculate_source") != NULL);
    assert(s_calculate_calls == 1U);
    assert(strcmp(s_last_calculate_session_id, "S000001") == 0);
    assert(s_last_calculate_config.propagation_speed_mps == 340.0);
    assert(s_last_calculate_config.propagation_speed_uncertainty_mps == 5.0);
    assert(strcmp(s_last_calculate_config.propagation_model_name, "tap_calibrated") == 0);
    assert(s_last_calculate_config.timing_uncertainty_us == 2000.0);
    assert(s_last_calculate_config.pick_config.noise_window_samples == 50U);
    assert(s_last_calculate_config.pick_config.sta_window_samples == 5U);
    assert(s_last_calculate_config.pick_config.lta_window_samples == 50U);
    assert(s_last_calculate_config.pick_config.sta_lta_trigger_ratio == 3.5f);
    assert(s_last_calculate_config.pick_config.refine_lookback_samples == 10U);
    assert(s_last_calculate_config.pick_config.refine_lookahead_samples == 10U);
    assert(s_last_calculate_config.pick_config.refine_amplitude_fraction == 0.5f);
    assert(s_last_calculate_config.pick_config.snr_signal_window_samples == 50U);
    assert(s_last_calculate_config.pick_config.minimum_snr == 2.0f);
    assert(s_last_calculate_config.semblance_window_half_span_samples == 25U);
    /* Optional max_source_depth_m absent: geometry-derived default (MAX-211). */
    assert(s_last_calculate_config.max_source_depth_m == 0.0);
    /* Optional S picking absent: PSAFUVS stays unattempted. */
    assert(!s_last_calculate_config.s_pick_enabled);
    /* No time budget given: the job's default (0). */
    assert(s_last_budget_ms == 0U);
    /* No object mutation occurred: object cache must not be touched. */
    assert(s_cache_invalidations == 0U);
    assert(s_cache_refresh_requests == 0U);
}

static void test_calculate_source_optional_max_depth(void)
{
    static const char *const with_depth =
        "{\"action\":\"calculate_source\",\"object_id\":\"OBJ_0001\",\"session_id\":\"S000001\","
        "\"propagation_speed_mps\":340.0,\"propagation_speed_uncertainty_mps\":5.0,"
        "\"timing_uncertainty_us\":2000.0,"
        "\"noise_window_samples\":50,\"sta_window_samples\":5,\"lta_window_samples\":50,"
        "\"sta_lta_trigger_ratio\":3.5,\"refine_lookback_samples\":10,\"refine_lookahead_samples\":10,"
        "\"refine_amplitude_fraction\":0.5,\"snr_signal_window_samples\":50,\"minimum_snr\":2.0,"
        "\"semblance_window_half_span_samples\":25,\"max_source_depth_m\":6.5}";
    reset_fakes();
    s_calculate_result = ESP_OK;
    char response[512] = {0};
    httpd_req_t request = make_request(with_depth, response, sizeof(response));
    assert(web_api_control_handler(&request) == ESP_OK);
    assert(request.response_status == NULL);
    assert(s_calculate_calls == 1U);
    assert(s_last_calculate_config.max_source_depth_m == 6.5);

    static const char *const bad_depth =
        "{\"action\":\"calculate_source\",\"object_id\":\"OBJ_0001\",\"session_id\":\"S000001\","
        "\"propagation_speed_mps\":340.0,\"propagation_speed_uncertainty_mps\":5.0,"
        "\"timing_uncertainty_us\":2000.0,"
        "\"noise_window_samples\":50,\"sta_window_samples\":5,\"lta_window_samples\":50,"
        "\"sta_lta_trigger_ratio\":3.5,\"refine_lookback_samples\":10,\"refine_lookahead_samples\":10,"
        "\"refine_amplitude_fraction\":0.5,\"snr_signal_window_samples\":50,\"minimum_snr\":2.0,"
        "\"semblance_window_half_span_samples\":25,\"max_source_depth_m\":-1.0}";
    reset_fakes();
    char bad_response[512] = {0};
    request = make_request(bad_depth, bad_response, sizeof(bad_response));
    assert(web_api_control_handler(&request) == ESP_OK);
    assert(request.response_status != NULL);
    assert(strcmp(request.response_status, "400 Bad Request") == 0);
    assert(s_calculate_calls == 0U);
}

/* PSAFUVS: the S-wave pick settings arrive as one complete, validated set. */
static void test_calculate_source_optional_s_wave_picking(void)
{
    static const char *const with_s =
        "{\"action\":\"calculate_source\",\"object_id\":\"OBJ_0001\",\"session_id\":\"S000001\","
        "\"propagation_speed_mps\":340.0,\"propagation_speed_uncertainty_mps\":5.0,"
        "\"timing_uncertainty_us\":2000.0,"
        "\"noise_window_samples\":50,\"sta_window_samples\":5,\"lta_window_samples\":50,"
        "\"sta_lta_trigger_ratio\":3.5,\"refine_lookback_samples\":10,\"refine_lookahead_samples\":10,"
        "\"refine_amplitude_fraction\":0.5,\"snr_signal_window_samples\":50,\"minimum_snr\":2.0,"
        "\"semblance_window_half_span_samples\":25,"
        "\"s_min_separation_samples\":20,\"s_max_separation_samples\":400,"
        "\"s_sta_window_samples\":10,\"s_lta_window_samples\":60,"
        "\"s_sta_lta_trigger_ratio\":3.0,\"s_minimum_snr\":2.0}";
    reset_fakes();
    s_calculate_result = ESP_OK;
    char response[512] = {0};
    httpd_req_t request = make_request(with_s, response, sizeof(response));
    assert(web_api_control_handler(&request) == ESP_OK);
    assert(request.response_status == NULL);
    assert(s_calculate_calls == 1U);
    assert(s_last_calculate_config.s_pick_enabled);
    assert(s_last_calculate_config.s_pick_config.min_separation_samples == 20U);
    assert(s_last_calculate_config.s_pick_config.max_separation_samples == 400U);
    assert(s_last_calculate_config.s_pick_config.sta_window_samples == 10U);
    assert(s_last_calculate_config.s_pick_config.lta_window_samples == 60U);
    assert(s_last_calculate_config.s_pick_config.sta_lta_trigger_ratio == 3.0f);
    assert(s_last_calculate_config.s_pick_config.minimum_snr == 2.0f);

    /* Incomplete (no s_minimum_snr) or unreachable (ratio >= lta/sta)
     * settings are refused, never completed with a guess. */
    static const char *const incomplete =
        "{\"action\":\"calculate_source\",\"object_id\":\"OBJ_0001\",\"session_id\":\"S000001\","
        "\"propagation_speed_mps\":340.0,\"propagation_speed_uncertainty_mps\":5.0,"
        "\"timing_uncertainty_us\":2000.0,"
        "\"noise_window_samples\":50,\"sta_window_samples\":5,\"lta_window_samples\":50,"
        "\"sta_lta_trigger_ratio\":3.5,\"refine_lookback_samples\":10,\"refine_lookahead_samples\":10,"
        "\"refine_amplitude_fraction\":0.5,\"snr_signal_window_samples\":50,\"minimum_snr\":2.0,"
        "\"semblance_window_half_span_samples\":25,"
        "\"s_min_separation_samples\":20,\"s_max_separation_samples\":400,"
        "\"s_sta_window_samples\":10,\"s_lta_window_samples\":60,\"s_sta_lta_trigger_ratio\":3.0}";
    static const char *const unreachable =
        "{\"action\":\"calculate_source\",\"object_id\":\"OBJ_0001\",\"session_id\":\"S000001\","
        "\"propagation_speed_mps\":340.0,\"propagation_speed_uncertainty_mps\":5.0,"
        "\"timing_uncertainty_us\":2000.0,"
        "\"noise_window_samples\":50,\"sta_window_samples\":5,\"lta_window_samples\":50,"
        "\"sta_lta_trigger_ratio\":3.5,\"refine_lookback_samples\":10,\"refine_lookahead_samples\":10,"
        "\"refine_amplitude_fraction\":0.5,\"snr_signal_window_samples\":50,\"minimum_snr\":2.0,"
        "\"semblance_window_half_span_samples\":25,"
        "\"s_min_separation_samples\":20,\"s_max_separation_samples\":400,"
        "\"s_sta_window_samples\":10,\"s_lta_window_samples\":60,"
        "\"s_sta_lta_trigger_ratio\":6.0,\"s_minimum_snr\":2.0}";
    const char *const bad[] = { incomplete, unreachable };
    for (size_t i = 0U; i < 2U; ++i) {
        reset_fakes();
        char bad_response[512] = {0};
        request = make_request(bad[i], bad_response, sizeof(bad_response));
        assert(web_api_control_handler(&request) == ESP_OK);
        assert(request.response_status != NULL);
        assert(strcmp(request.response_status, "400 Bad Request") == 0);
        assert(s_calculate_calls == 0U);
    }
}

/* MAX-210: sta=10, lta=50 bounds STA/LTA at 5.0; a 6.0 threshold could
 * never trigger and must be rejected rather than dispatched. */
static void test_calculate_source_rejects_unreachable_sta_lta_threshold(void)
{
    static const char *const body =
        "{\"action\":\"calculate_source\",\"object_id\":\"OBJ_0001\",\"session_id\":\"S000001\","
        "\"propagation_speed_mps\":340.0,\"propagation_speed_uncertainty_mps\":5.0,"
        "\"timing_uncertainty_us\":2000.0,"
        "\"noise_window_samples\":50,\"sta_window_samples\":10,\"lta_window_samples\":50,"
        "\"sta_lta_trigger_ratio\":6.0,\"refine_lookback_samples\":10,\"refine_lookahead_samples\":10,"
        "\"refine_amplitude_fraction\":0.5,\"snr_signal_window_samples\":50,\"minimum_snr\":2.0,"
        "\"semblance_window_half_span_samples\":25}";
    reset_fakes();
    char response[512] = {0};
    httpd_req_t request = make_request(body, response, sizeof(response));
    assert(web_api_control_handler(&request) == ESP_OK);
    assert(request.response_status != NULL);
    assert(strcmp(request.response_status, "400 Bad Request") == 0);
    assert(s_calculate_calls == 0U);
}

static void test_calculate_source_action_rejects_missing_session_id(void)
{
    reset_fakes();
    const char *body =
        "{\"action\":\"calculate_source\",\"object_id\":\"OBJ_0001\","
        "\"propagation_speed_mps\":340.0,\"propagation_speed_uncertainty_mps\":5.0,"
        "\"timing_uncertainty_us\":2000.0}";
    char response[512] = {0};
    httpd_req_t request = make_request(body, response, sizeof(response));

    assert(web_api_control_handler(&request) == ESP_OK);
    assert(request.response_status != NULL);
    assert(strcmp(request.response_status, "400 Bad Request") == 0);
    assert(strstr(response, "\"ok\":false") != NULL);
    assert(s_calculate_calls == 0U);
}

static void test_save_calc_settings_validates_and_persists_only_settings(void)
{
    static const char *const valid =
        "{\"action\":\"save_calc_settings\",\"object_id\":\"OBJ_0001\","
        "\"propagation_speed_mps\":340.0,\"propagation_speed_uncertainty_mps\":5.0,"
        "\"propagation_model_name\":\"tap_calibrated\",\"timing_uncertainty_us\":2000.0,"
        "\"noise_window_samples\":50,\"sta_window_samples\":5,\"lta_window_samples\":50,"
        "\"sta_lta_trigger_ratio\":3.5,\"refine_lookback_samples\":10,"
        "\"refine_lookahead_samples\":10,\"refine_amplitude_fraction\":0.5,"
        "\"snr_signal_window_samples\":50,\"minimum_snr\":2.0,"
        "\"semblance_window_half_span_samples\":25,\"time_budget_s\":45.5,"
        "\"ignored_extension\":\"discard me\"}";
    reset_fakes();
    char response[512] = {0};
    httpd_req_t request = make_request(valid, response, sizeof(response));
    assert(web_api_control_handler(&request) == ESP_OK);
    assert(request.response_status == NULL);
    assert(s_calc_settings_save_calls == 1U);
    assert(s_calc_settings_notify_calls == 1U);
    assert(strstr(s_saved_calc_settings, "\"action\"") == NULL);
    assert(strstr(s_saved_calc_settings, "\"object_id\"") == NULL);
    assert(strstr(s_saved_calc_settings, "\"ignored_extension\"") == NULL);
    assert(strstr(s_saved_calc_settings, "\"propagation_speed_mps\":340") != NULL);
    assert(strstr(s_saved_calc_settings, "\"material\":\"custom\"") != NULL);
    assert(strstr(s_saved_calc_settings, "\"time_budget_s\":45.5") != NULL);

    const char *saved_before_invalid = s_saved_calc_settings;
    char saved_copy[sizeof(s_saved_calc_settings)];
    (void)snprintf(saved_copy, sizeof(saved_copy), "%s", saved_before_invalid);
    static const char *const invalid =
        "{\"action\":\"save_calc_settings\",\"object_id\":\"OBJ_0001\","
        "\"propagation_speed_mps\":340.0,\"propagation_speed_uncertainty_mps\":5.0,"
        "\"timing_uncertainty_us\":2000.0,\"noise_window_samples\":50,"
        "\"sta_window_samples\":10,\"lta_window_samples\":50,\"sta_lta_trigger_ratio\":6.0,"
        "\"refine_lookback_samples\":10,\"refine_lookahead_samples\":10,"
        "\"refine_amplitude_fraction\":0.5,\"snr_signal_window_samples\":50,"
        "\"minimum_snr\":2.0,\"semblance_window_half_span_samples\":25}";
    memset(response, 0, sizeof(response));
    request = make_request(invalid, response, sizeof(response));
    assert(web_api_control_handler(&request) == ESP_OK);
    assert(request.response_status != NULL && strcmp(request.response_status, "400 Bad Request") == 0);
    assert(s_calc_settings_save_calls == 1U);
    assert(s_calc_settings_notify_calls == 1U);
    assert(strcmp(s_saved_calc_settings, saved_copy) == 0);
}

static void test_save_material_profiles_resolve_defaults_and_advanced_overrides(void)
{
    static const char *const preset_override =
        "{\"action\":\"save_calc_settings\",\"object_id\":\"OBJ_0001\",\"material\":\"concrete\","
        "\"propagation_speed_mps\":90,\"propagation_speed_uncertainty_mps\":1,"
        "\"timing_uncertainty_us\":777}";
    reset_fakes();
    char response[512] = {0};
    httpd_req_t request = make_request(preset_override, response, sizeof(response));
    assert(web_api_control_handler(&request) == ESP_OK);
    assert(request.response_status == NULL && s_calc_settings_save_calls == 1U);
    cJSON *saved = cJSON_Parse(s_saved_calc_settings);
    assert(saved != NULL);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(saved, "material")->valuestring, "concrete") == 0);
    assert(cJSON_GetObjectItemCaseSensitive(saved, "propagation_speed_mps")->valuedouble == 4000.0);
    assert(cJSON_GetObjectItemCaseSensitive(saved,
        "propagation_speed_uncertainty_mps")->valuedouble == 500.0);
    assert(cJSON_GetObjectItemCaseSensitive(saved, "timing_uncertainty_us")->valuedouble == 777.0);
    assert(cJSON_GetObjectItemCaseSensitive(saved, "noise_window_samples")->valuedouble == 50.0);
    assert(cJSON_GetObjectItemCaseSensitive(saved, "sta_window_samples")->valuedouble == 10.0);
    assert(cJSON_GetObjectItemCaseSensitive(saved, "lta_window_samples")->valuedouble == 100.0);
    assert(cJSON_GetObjectItemCaseSensitive(saved, "s_pick_enabled") != NULL &&
        !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(saved, "s_pick_enabled")));
    assert(cJSON_GetObjectItemCaseSensitive(saved, "time_budget_s")->valuedouble == 60.0);
    cJSON_Delete(saved);

    static const char *const custom =
        "{\"action\":\"save_calc_settings\",\"object_id\":\"OBJ_0001\",\"material\":\"custom\","
        "\"propagation_speed_mps\":340,\"propagation_speed_uncertainty_mps\":12}";
    reset_fakes();
    memset(response, 0, sizeof(response));
    request = make_request(custom, response, sizeof(response));
    assert(web_api_control_handler(&request) == ESP_OK);
    assert(request.response_status == NULL && s_calc_settings_save_calls == 1U);
    saved = cJSON_Parse(s_saved_calc_settings);
    assert(saved != NULL);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(saved, "material")->valuestring, "custom") == 0);
    assert(cJSON_GetObjectItemCaseSensitive(saved, "propagation_speed_mps")->valuedouble == 340.0);
    assert(cJSON_GetObjectItemCaseSensitive(saved,
        "propagation_speed_uncertainty_mps")->valuedouble == 12.0);
    assert(cJSON_GetObjectItemCaseSensitive(saved, "minimum_snr")->valuedouble == 3.0);
    cJSON_Delete(saved);

    static const char *const explicit_preset =
        "{\"action\":\"save_calc_settings\",\"object_id\":\"OBJ_0001\","
        "\"propagation_speed_mps\":2200,\"propagation_speed_uncertainty_mps\":700,"
        "\"timing_uncertainty_us\":2000,\"noise_window_samples\":50,"
        "\"sta_window_samples\":10,\"lta_window_samples\":100,\"sta_lta_trigger_ratio\":3,"
        "\"refine_lookback_samples\":10,\"refine_lookahead_samples\":10,"
        "\"refine_amplitude_fraction\":0.5,\"snr_signal_window_samples\":20,"
        "\"minimum_snr\":3,\"semblance_window_half_span_samples\":10}";
    reset_fakes();
    memset(response, 0, sizeof(response));
    request = make_request(explicit_preset, response, sizeof(response));
    assert(web_api_control_handler(&request) == ESP_OK);
    assert(request.response_status == NULL && s_calc_settings_save_calls == 1U);
    saved = cJSON_Parse(s_saved_calc_settings);
    assert(saved != NULL);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(saved, "material")->valuestring, "brick_masonry") == 0);
    assert(cJSON_GetObjectItemCaseSensitive(saved, "propagation_speed_mps")->valuedouble == 2200.0);
    cJSON_Delete(saved);
}

static void test_save_calc_material_rejects_unknown_and_custom_bounds(void)
{
    static const char *const invalid[] = {
        "{\"action\":\"save_calc_settings\",\"object_id\":\"OBJ_0001\",\"material\":\"unknown\"}",
        "{\"action\":\"save_calc_settings\",\"object_id\":\"OBJ_0001\",\"material\":\"custom\"}",
        "{\"action\":\"save_calc_settings\",\"object_id\":\"OBJ_0001\",\"material\":\"custom\",\"propagation_speed_mps\":50,\"propagation_speed_uncertainty_mps\":1}",
        "{\"action\":\"save_calc_settings\",\"object_id\":\"OBJ_0001\",\"material\":\"custom\",\"propagation_speed_mps\":8000,\"propagation_speed_uncertainty_mps\":1}",
        "{\"action\":\"save_calc_settings\",\"object_id\":\"OBJ_0001\",\"material\":\"custom\",\"propagation_speed_mps\":340,\"propagation_speed_uncertainty_mps\":0}",
        "{\"action\":\"save_calc_settings\",\"object_id\":\"OBJ_0001\",\"material\":\"custom\",\"propagation_speed_mps\":340,\"propagation_speed_uncertainty_mps\":340}",
    };
    for (size_t i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        reset_fakes();
        char response[512] = {0};
        httpd_req_t request = make_request(invalid[i], response, sizeof(response));
        assert(web_api_control_handler(&request) == ESP_OK);
        assert(request.response_status != NULL && strcmp(request.response_status, "400 Bad Request") == 0);
        assert(s_calc_settings_save_calls == 0U);
        assert(s_calc_settings_notify_calls == 0U);
    }
}

static void test_calculate_source_time_budget(void)
{
    static const char *const prefix =
        "{\"action\":\"calculate_source\",\"object_id\":\"OBJ_0001\",\"session_id\":\"S000001\","
        "\"propagation_speed_mps\":340.0,\"propagation_speed_uncertainty_mps\":5.0,"
        "\"timing_uncertainty_us\":2000.0,"
        "\"noise_window_samples\":50,\"sta_window_samples\":5,\"lta_window_samples\":50,"
        "\"sta_lta_trigger_ratio\":3.5,\"refine_lookback_samples\":10,\"refine_lookahead_samples\":10,"
        "\"refine_amplitude_fraction\":0.5,\"snr_signal_window_samples\":50,\"minimum_snr\":2.0,"
        "\"semblance_window_half_span_samples\":25,\"time_budget_s\":";
    char body[1024];
    char response[512];

    reset_fakes();
    snprintf(body, sizeof(body), "%s%s}", prefix, "45.5");
    memset(response, 0, sizeof(response));
    httpd_req_t request = make_request(body, response, sizeof(response));
    assert(web_api_control_handler(&request) == ESP_OK);
    assert(request.response_status == NULL && s_calculate_calls == 1U);
    assert(s_last_budget_ms == 45500U);

    static const char *const invalid[] = { "0.5", "601", "\"60\"", "-3" };
    for (size_t i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        reset_fakes();
        snprintf(body, sizeof(body), "%s%s}", prefix, invalid[i]);
        memset(response, 0, sizeof(response));
        request = make_request(body, response, sizeof(response));
        assert(web_api_control_handler(&request) == ESP_OK);
        assert(request.response_status != NULL && strcmp(request.response_status, "400 Bad Request") == 0);
        assert(s_calculate_calls == 0U);
    }
}

/* A job already queued or running: refused with 409, never queued. */
static void test_calculate_source_action_surfaces_solver_failure(void)
{
    reset_fakes();
    s_calculate_result = ESP_ERR_INVALID_STATE;
    char response[512] = {0};
    httpd_req_t request = make_request(CALCULATE_SOURCE_BODY, response, sizeof(response));

    assert(web_api_control_handler(&request) == ESP_OK);
    assert(request.response_status != NULL);
    assert(strcmp(request.response_status, "409 Conflict") == 0);
    assert(strstr(response, "\"ok\":false") != NULL);
    assert(s_calculate_calls == 1U);
}

static void test_activate_object_clears_staging_for_previous_object(void)
{
    reset_fakes();
    char response[512] = {0};
    httpd_req_t request = make_request(
        "{\"action\":\"activate_object\",\"object_id\":\"OBJ_0002\"}",
        response,
        sizeof(response)
    );

    assert(web_api_control_handler(&request) == ESP_OK);
    assert(request.response_status == NULL);
    assert(s_set_active_calls == 1U);
    assert(strcmp(s_last_set_active_id, "OBJ_0002") == 0);
    assert(s_clear_staging_calls == 1U);
    assert(strcmp(s_cleared_staging_id, "OBJ_0001") == 0);
}

static void test_activate_object_does_not_clear_staging_when_activation_fails(void)
{
    reset_fakes();
    s_set_active_result = ESP_FAIL;
    char response[512] = {0};
    httpd_req_t request = make_request(
        "{\"action\":\"activate_object\",\"object_id\":\"OBJ_0002\"}",
        response,
        sizeof(response)
    );

    assert(web_api_control_handler(&request) == ESP_OK);
    assert(s_set_active_calls == 1U);
    assert(s_clear_staging_calls == 0U);
}

static void test_external_workflow_commands_require_generation(void)
{
    for (int command = 0; command < WORKFLOW_COMMAND_COUNT; ++command) {
        reset_fakes();
        char body[256], response[512] = {0};
        (void)snprintf(body, sizeof(body), "{\"action\":\"workflow_command\",\"command\":\"%s\"}",
            workflow_core_command_name((workflow_command_t)command));
        httpd_req_t request = make_request(body, response, sizeof(response));
        assert(web_api_control_handler(&request) == ESP_OK);
        assert(request.response_status != NULL && strcmp(request.response_status, "400 Bad Request") == 0);
        assert(s_workflow_command_calls == 0U);
    }
}

static void test_workflow_command_success_and_conflict_responses(void)
{
    static const char *const success_body =
        "{\"action\":\"workflow_command\",\"command\":\"BACK\",\"generation\":12,"
        "\"ok\":true,\"reason\":\"operator_back\"}";
    reset_fakes();
    s_workflow_snapshot.step = WORKFLOW_STEP_SENSOR_CHECK;
    s_workflow_snapshot.generation = 13U;
    char response[512] = {0};
    httpd_req_t request = make_request(success_body, response, sizeof(response));
    assert(web_api_control_handler(&request) == ESP_OK);
    assert(request.response_status == NULL);
    assert(s_workflow_command_calls == 1U && s_workflow_snapshot_calls == 1U);
    assert(s_workflow_command == WORKFLOW_COMMAND_BACK);
    assert(s_workflow_params.has_generation && s_workflow_params.expected_generation == 12U);
    assert(s_workflow_params.has_ok && s_workflow_params.ok);
    assert(strcmp(s_workflow_reason, "operator_back") == 0);
    cJSON *body = cJSON_Parse(response);
    assert(body != NULL && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body, "ok")));
    cJSON *workflow = cJSON_GetObjectItemCaseSensitive(body, "workflow");
    assert(cJSON_IsObject(workflow));
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(workflow, "step")->valuestring, "SENSOR_CHECK") == 0);
    assert(cJSON_GetObjectItemCaseSensitive(workflow, "generation")->valuedouble == 13.0);
    cJSON_Delete(body);

    reset_fakes();
    s_workflow_result = WORKFLOW_COMMAND_RESULT_NOT_ALLOWED;
    request = make_request(
        "{\"action\":\"workflow_command\",\"command\":\"BACK\",\"generation\":1}",
        response,
        sizeof(response)
    );
    assert(web_api_control_handler(&request) == ESP_OK);
    assert(request.response_status != NULL && strcmp(request.response_status, "409 Conflict") == 0);
    body = cJSON_Parse(response);
    assert(body != NULL && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body, "ok")));
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(body, "reason")->valuestring, "not_allowed") == 0);
    assert(cJSON_GetObjectItemCaseSensitive(body, "workflow") == NULL);
    cJSON_Delete(body);

    reset_fakes();
    s_workflow_result = WORKFLOW_COMMAND_RESULT_STALE;
    request = make_request(
        "{\"action\":\"workflow_command\",\"command\":\"BACK\",\"generation\":3}",
        response,
        sizeof(response)
    );
    assert(web_api_control_handler(&request) == ESP_OK);
    assert(request.response_status != NULL && strcmp(request.response_status, "409 Conflict") == 0);
    body = cJSON_Parse(response);
    assert(body != NULL);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(body, "reason")->valuestring, "stale") == 0);
    cJSON_Delete(body);
}

static void test_manual_distance_payload_is_strict_and_generation_scoped(void)
{
    reset_fakes();
    char response[1024] = {0};
    httpd_req_t request = make_request(
        "{\"action\":\"workflow_command\",\"command\":\"SET_DISTANCE\",\"generation\":12,"
        "\"node_a\":0,\"node_b\":2,\"distance_mm\":1500,\"ranging_started_at_ms\":100,\"ranging_updated_at_ms\":200}",
        response, sizeof(response));
    assert(web_api_control_handler(&request) == ESP_OK);
    assert(s_workflow_command_calls == 1U && s_workflow_command == WORKFLOW_COMMAND_SET_DISTANCE);
    assert(s_workflow_params.has_generation && s_workflow_params.expected_generation == 12U);
    assert(s_workflow_params.has_distance && s_workflow_params.node_a == 0U && s_workflow_params.node_b == 2U);
    assert(s_workflow_params.distance_mm == 1500 && s_workflow_params.ranging_started_at_ms == 100U);
    assert(s_workflow_params.ranging_updated_at_ms == 200U);
    static const char *bad_values[] = {"0", "-1", "1.5", "2147483648", "null", "true", "\"1500\""};
    for (size_t i = 0U; i < sizeof(bad_values) / sizeof(bad_values[0]); ++i) {
        reset_fakes();
        char body[320];
        (void)snprintf(body, sizeof(body),
            "{\"action\":\"workflow_command\",\"command\":\"SET_DISTANCE\",\"generation\":12,"
            "\"node_a\":0,\"node_b\":2,\"distance_mm\":%s,\"ranging_started_at_ms\":100,\"ranging_updated_at_ms\":200}", bad_values[i]);
        memset(response, 0, sizeof(response));
        request = make_request(body, response, sizeof(response));
        assert(web_api_control_handler(&request) == ESP_OK);
        assert(request.response_status != NULL && strcmp(request.response_status, "400 Bad Request") == 0);
        assert(s_workflow_command_calls == 0U);
    }
    static const char *bad_payloads[] = {
        "{\"node_a\":0,\"node_b\":2,\"distance_mm\":1500,\"ranging_started_at_ms\":100,\"ranging_updated_at_ms\":200}",
        "{\"generation\":12,\"node_a\":0,\"node_b\":0,\"distance_mm\":1500,\"ranging_started_at_ms\":100,\"ranging_updated_at_ms\":200}",
        "{\"generation\":12,\"node_a\":0,\"node_b\":2.5,\"distance_mm\":1500,\"ranging_started_at_ms\":100,\"ranging_updated_at_ms\":200}",
        "{\"generation\":12,\"node_a\":0,\"node_b\":2,\"distance_mm\":1500,\"ranging_started_at_ms\":-1,\"ranging_updated_at_ms\":200}",
        "{\"generation\":12,\"node_a\":0,\"node_b\":2,\"distance_mm\":1500,\"ranging_started_at_ms\":100}",
    };
    for (size_t i = 0U; i < sizeof(bad_payloads) / sizeof(bad_payloads[0]); ++i) {
        reset_fakes();
        char body[512];
        (void)snprintf(body, sizeof(body), "{\"action\":\"workflow_command\",\"command\":\"SET_DISTANCE\",%s", bad_payloads[i] + 1);
        request = make_request(body, response, sizeof(response));
        assert(web_api_control_handler(&request) == ESP_OK);
        assert(s_workflow_command_calls == 0U);
        assert(request.response_status != NULL && strcmp(request.response_status, "400 Bad Request") == 0);
    }
}

static void test_workflow_command_rejects_unknown_name_before_dispatch(void)
{
    reset_fakes();
    char response[512] = {0};
    httpd_req_t request = make_request(
        "{\"action\":\"workflow_command\",\"command\":\"NOT_A_COMMAND\"}",
        response,
        sizeof(response)
    );
    assert(web_api_control_handler(&request) == ESP_OK);
    assert(request.response_status != NULL && strcmp(request.response_status, "400 Bad Request") == 0);
    assert(s_workflow_command_calls == 0U);
    assert(strstr(response, "ESP_ERR_INVALID_ARG") != NULL);
}

static void test_workflow_gps_accept_parses_fix_and_optional_altitude(void)
{
    reset_fakes();
    char response[512] = {0};
    httpd_req_t request = make_request(
        "{\"action\":\"workflow_command\",\"command\":\"GPS_ACCEPT\",\"generation\":9,"
        "\"latitude\":50.4501,\"longitude\":30.5234,\"accuracy_m\":3.2,"
        "\"captured_at_ms\":123456789,\"altitude_m\":179.4,\"altitude_accuracy_m\":5.5}",
        response,
        sizeof(response)
    );
    assert(web_api_control_handler(&request) == ESP_OK);
    assert(request.response_status == NULL);
    assert(s_workflow_command_calls == 1U);
    assert(s_workflow_command == WORKFLOW_COMMAND_GPS_ACCEPT);
    assert(s_workflow_params.has_generation && s_workflow_params.expected_generation == 9U);
    assert(s_workflow_params.has_location);
    assert(s_workflow_params.latitude == 50.4501);
    assert(s_workflow_params.longitude == 30.5234);
    assert(s_workflow_params.accuracy_m == 3.2);
    assert(s_workflow_params.captured_at_ms == UINT64_C(123456789));
    assert(s_workflow_params.altitude_valid);
    assert(s_workflow_params.altitude_m == 179.4);
    assert(s_workflow_params.altitude_accuracy_m == 5.5);

    reset_fakes();
    request = make_request(
        "{\"action\":\"workflow_command\",\"command\":\"GPS_ACCEPT\",\"generation\":10,"
        "\"latitude\":-90,\"longitude\":180,\"accuracy_m\":0,\"captured_at_ms\":0}",
        response,
        sizeof(response)
    );
    assert(web_api_control_handler(&request) == ESP_OK);
    assert(request.response_status == NULL);
    assert(s_workflow_command_calls == 1U);
    assert(s_workflow_command == WORKFLOW_COMMAND_GPS_ACCEPT);
    assert(s_workflow_params.has_location && !s_workflow_params.altitude_valid);
}

static void test_workflow_gps_accept_rejects_malformed_payload_before_dispatch(void)
{
    static const char *const bodies[] = {
        "{\"action\":\"workflow_command\",\"command\":\"GPS_ACCEPT\",\"generation\":1,"
            "\"longitude\":20,\"accuracy_m\":1,\"captured_at_ms\":2}",
        "{\"action\":\"workflow_command\",\"command\":\"GPS_ACCEPT\",\"generation\":1,"
            "\"latitude\":10,\"longitude\":20,\"accuracy_m\":1,\"captured_at_ms\":2,"
            "\"altitude_accuracy_m\":3}",
        "{\"action\":\"workflow_command\",\"command\":\"GPS_ACCEPT\",\"generation\":1,"
            "\"latitude\":10,\"longitude\":20,\"accuracy_m\":1,\"captured_at_ms\":2.5}",
        "{\"action\":\"workflow_command\",\"command\":\"GPS_ACCEPT\","
            "\"latitude\":10,\"longitude\":20,\"accuracy_m\":1,\"captured_at_ms\":2}",
        "{\"action\":\"workflow_command\",\"command\":\"GPS_CAPTURE_FAILED\","
            "\"reason\":\"permission_denied\"}",
    };
    for (size_t index = 0U; index < sizeof(bodies) / sizeof(bodies[0]); ++index) {
        reset_fakes();
        char response[512] = {0};
        httpd_req_t request = make_request(bodies[index], response, sizeof(response));
        assert(web_api_control_handler(&request) == ESP_OK);
        assert(request.response_status != NULL && strcmp(request.response_status, "400 Bad Request") == 0);
        assert(s_workflow_command_calls == 0U);
    }
}

static void test_workflow_command_rejects_internal_completion_commands(void)
{
    static const struct {
        const char *name;
        workflow_step_t step;
    } cases[] = {
        { "CALIBRATION_DONE", WORKFLOW_STEP_CALIBRATING },
        { "RANGING_DONE", WORKFLOW_STEP_RANGING },
        { "CALCULATION_DONE", WORKFLOW_STEP_CALCULATING },
    };

    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        reset_fakes();
        s_workflow_snapshot.step = cases[i].step;
        char request_body[192];
        (void)snprintf(request_body, sizeof(request_body),
            "{\"action\":\"workflow_command\",\"command\":\"%s\",\"ok\":true}",
            cases[i].name);
        char response[512] = {0};
        httpd_req_t request = make_request(request_body, response, sizeof(response));
        assert(web_api_control_handler(&request) == ESP_OK);
        assert(request.response_status != NULL &&
            strcmp(request.response_status, "400 Bad Request") == 0);
        assert(s_workflow_command_calls == 0U);
        assert(s_workflow_snapshot_calls == 0U);
        assert(s_workflow_snapshot.step == cases[i].step);

        cJSON *body = cJSON_Parse(response);
        assert(body != NULL && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body, "ok")));
        assert(strcmp(cJSON_GetObjectItemCaseSensitive(body, "reason")->valuestring,
            "internal_command") == 0);
        cJSON_Delete(body);
    }
}

int main(void)
{
    assert(storage_bulk_admission_initialize() == ESP_OK);
    test_scan_recording_gate_precedes_object_lease();
    test_measurement_start_requires_workflow_step();
    test_measurement_start_rejects_acquisition_failures();
    test_measurement_start_success_stays_async();
    test_measurement_start_async_failure_keeps_existing_mapping();
    test_calibrate_heights_rejects_active_measurements();
    test_calibrate_heights_rejects_no_remotes();
    test_calibrate_heights_queues_channel_mask_without_bus_io();
    {
        char response[512] = {0};
        reset_fakes();
        httpd_req_t request = make_request("{\"action\":\"scan_remote_sensors\",\"channel\":2}", response, sizeof(response));
        assert(web_api_control_handler(&request) == ESP_OK);
        assert(s_sensor_scan_calls == 1U && s_sensor_scan_channel == 2U);
        cJSON *body = cJSON_Parse(response);
        assert(body != NULL && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body, "ok")));
        assert(cJSON_GetObjectItemCaseSensitive(body, "request_id")->valueint == 1234);
        cJSON_Delete(body);
        request = make_request("{\"action\":\"sensor_scan_dispatch\",\"request_id\":1234}", response, sizeof(response));
        assert(web_api_control_handler(&request) == ESP_OK);
        body = cJSON_Parse(response);
        assert(body != NULL && cJSON_GetObjectItemCaseSensitive(body, "pending") != NULL &&
               !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body, "pending")));
        assert(cJSON_GetObjectItemCaseSensitive(body, "dispatch_error")->valueint == ESP_FAIL);
        cJSON_Delete(body);
        request = make_request("{\"action\":\"scan_remote_sensors\",\"channel\":2.5}", response, sizeof(response));
        assert(web_api_control_handler(&request) == ESP_OK);
        assert(s_sensor_scan_calls == 1U);
        assert(request.response_status != NULL && strcmp(request.response_status, "400 Bad Request") == 0);
    }
    test_phone_location_handler_round_trip();
    test_phone_location_handler_rejects_negative_capture_time();
    test_phone_location_handler_rejects_unrepresentable_capture_time();
    test_control_response_is_complete_or_not_sent_under_allocation_failure();
    test_activate_object_clears_staging_for_previous_object();
    test_activate_object_does_not_clear_staging_when_activation_fails();
    test_calculate_source_action_submits_a_background_job();
    test_calculate_source_optional_max_depth();
    test_calculate_source_optional_s_wave_picking();
    test_calculate_source_rejects_unreachable_sta_lta_threshold();
    test_calculate_source_action_rejects_missing_session_id();
    test_save_calc_settings_validates_and_persists_only_settings();
    test_save_material_profiles_resolve_defaults_and_advanced_overrides();
    test_save_calc_material_rejects_unknown_and_custom_bounds();
    test_calculate_source_action_surfaces_solver_failure();
    test_calculate_source_time_budget();
    test_external_workflow_commands_require_generation();
    test_workflow_command_success_and_conflict_responses();
    test_manual_distance_payload_is_strict_and_generation_scoped();
    test_workflow_gps_accept_parses_fix_and_optional_altitude();
    test_workflow_gps_accept_rejects_malformed_payload_before_dispatch();
    test_workflow_command_rejects_unknown_name_before_dispatch();
    test_workflow_command_rejects_internal_completion_commands();
    return 0;
}
