#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "localization_calculate.h"
#include "measurement_service.h"
#include "object_store.h"
#include "web_server_internal.h"

static object_store_location_t s_saved_location;
static unsigned s_location_calls;
static unsigned s_cache_invalidations;
static unsigned s_cache_refresh_requests;
static unsigned s_calculate_calls;
static localization_calculate_config_t s_last_calculate_config;
static char s_last_calculate_session_id[32];
static esp_err_t s_calculate_result;

static void reset_fakes(void)
{
    memset(&s_saved_location, 0, sizeof(s_saved_location));
    s_location_calls = 0U;
    s_cache_invalidations = 0U;
    s_cache_refresh_requests = 0U;
    s_calculate_calls = 0U;
    memset(&s_last_calculate_config, 0, sizeof(s_last_calculate_config));
    memset(s_last_calculate_session_id, 0, sizeof(s_last_calculate_session_id));
    s_calculate_result = ESP_OK;
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
    return ESP_OK;
}

esp_err_t measurement_service_start_object_async(const char *object_id)
{
    (void)object_id;
    return ESP_OK;
}

esp_err_t measurement_service_stop_async(void)
{
    return ESP_OK;
}

esp_err_t seismic_acquisition_start(void)
{
    return ESP_OK;
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

esp_err_t object_store_set_active(const char *object_id)
{
    (void)object_id;
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
    if (lease != NULL) lease->token_id = 1U;
    return ESP_OK;
}

esp_err_t object_store_lease_release(object_store_lease_t *lease)
{
    (void)lease;
    return ESP_OK;
}

int64_t esp_timer_get_time(void)
{
    return 1000000;
}

esp_err_t localization_calculate_run(
    const char *object_id,
    const char *session_id,
    const localization_calculate_config_t *config,
    uint64_t computed_at_ms,
    localization_calculate_summary_t *summary_out
)
{
    (void)object_id;
    (void)computed_at_ms;
    assert(config != NULL && summary_out != NULL);
    s_calculate_calls++;
    s_last_calculate_config = *config;
    snprintf(s_last_calculate_session_id, sizeof(s_last_calculate_session_id), "%s", session_id);
    memset(summary_out, 0, sizeof(*summary_out));
    if (s_calculate_result == ESP_OK) {
        summary_out->participating_channel_count = 4U;
        summary_out->methods_attempted = 4U;
        summary_out->methods_solved = 3U;
        summary_out->methods_persisted = 5U;
    }
    return s_calculate_result;
}

bool object_format_id_is_valid(const char *object_id)
{
    return object_id != NULL && strcmp(object_id, "OBJ_0001") == 0;
}

esp_err_t web_scan_store_try_handle(httpd_req_t *request, bool *handled)
{
    (void)request;
    if (handled != NULL) *handled = false;
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
    (void)query;
    (void)key;
    (void)value;
    (void)value_size;
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

static const char *const CALCULATE_SOURCE_BODY =
    "{\"action\":\"calculate_source\",\"object_id\":\"OBJ_0001\",\"session_id\":\"S000001\","
    "\"propagation_speed_mps\":340.0,\"propagation_speed_uncertainty_mps\":5.0,"
    "\"propagation_model_name\":\"tap_calibrated\",\"timing_uncertainty_us\":2000.0,"
    "\"noise_window_samples\":50,\"sta_window_samples\":5,\"lta_window_samples\":50,"
    "\"sta_lta_trigger_ratio\":3.5,\"refine_lookback_samples\":10,\"refine_lookahead_samples\":10,"
    "\"refine_amplitude_fraction\":0.5,\"snr_signal_window_samples\":50,\"minimum_snr\":2.0,"
    "\"semblance_window_half_span_samples\":25}";

static void test_calculate_source_action_dispatches_to_localization_calculate_run(void)
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
    /* No object mutation occurred: object cache must not be touched. */
    assert(s_cache_invalidations == 0U);
    assert(s_cache_refresh_requests == 0U);
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

int main(void)
{
    test_phone_location_handler_round_trip();
    test_phone_location_handler_rejects_negative_capture_time();
    test_calculate_source_action_dispatches_to_localization_calculate_run();
    test_calculate_source_action_rejects_missing_session_id();
    test_calculate_source_action_surfaces_solver_failure();
    return 0;
}
