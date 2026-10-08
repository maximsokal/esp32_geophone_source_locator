#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "cJSON.h"
#include "localization_run_record.h"
#include "localization_run_store.h"
#include "object_store.h"
#include "seismic_recorder.h"
#include "web_server_internal.h"

static unsigned s_list_calls;
static esp_err_t s_list_result;
static size_t s_list_record_count;
static localization_run_store_list_status_t s_list_status;
static char s_list_object_id[OBJECT_STORE_ID_SIZE];
static char s_list_session_id[LOCALIZATION_RUN_SESSION_ID_SIZE];
static unsigned s_geometry_calls;
static esp_err_t s_geometry_result;
static unsigned s_participation_calls;
static esp_err_t s_participation_result;
static seismic_recorder_session_participation_t s_participation;
static unsigned s_cs_pairs_calls;
static esp_err_t s_cs_pairs_result;
static unsigned s_picks_list_calls;
static esp_err_t s_picks_list_result;
static size_t s_picks_record_count;
static localization_run_store_list_status_t s_picks_list_status;
static esp_err_t s_calc_settings_result;
static const char *s_calc_settings_json;

static void reset_fakes(void)
{
    s_list_calls = 0U;
    s_list_result = ESP_OK;
    s_list_record_count = 2U;
    memset(&s_list_status, 0, sizeof(s_list_status));
    memset(s_list_object_id, 0, sizeof(s_list_object_id));
    memset(s_list_session_id, 0, sizeof(s_list_session_id));
    s_geometry_calls = 0U;
    s_geometry_result = ESP_OK;
    s_participation_calls = 0U;
    s_participation_result = ESP_OK;
    memset(&s_participation, 0, sizeof(s_participation));
    s_participation.pods[0] = (seismic_recorder_pod_participation_t) {
        .assigned = true,
        .node_id_known = true,
        .node_id = 42U,
        .hardware_id_length = 3U,
        .hardware_id = {0x01U, 0xABU, 0xFFU},
        .position_source = SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING_BARO,
    };
    s_participation.pods[1] = (seismic_recorder_pod_participation_t) {
        .assigned = true,
        .position_source = SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING_BARO,
    };
    s_participation.z_provenance.available = true;
    s_participation.z_provenance.effective_policy = SEISMIC_RECORDER_GEOMETRY_Z_BAROMETRIC_RELATIVE;
    s_participation.geometry_provenance = (seismic_recorder_geometry_provenance_t) {
        .available = true,
        .solver = "nrf_geometry_solver",
        .solver_version = "1",
        .refined = true,
        .refine_iterations = 3U,
        .topology_generation = 143U,
        .cs_membership_generation = 52U,
        .main_height_reference_generation = 7U,
        .setup_id = "1000-143-52-7",
        .start_uptime_ms = 995U,
    };
    s_participation.pods[0].source_sample_uptime_ms = 980U;
    s_participation.pods[0].esp_receipt_uptime_ms = 990U;
    s_participation.pods[0].source_age_at_start_known = true;
    s_participation.pods[0].source_age_ms_at_start = 5U;
    s_participation.pods[0].height_reference_generation = 7U;
    s_cs_pairs_calls = 0U;
    s_cs_pairs_result = ESP_OK;
    s_picks_list_calls = 0U;
    s_picks_list_result = ESP_OK;
    s_picks_record_count = 1U;
    memset(&s_picks_list_status, 0, sizeof(s_picks_list_status));
    s_calc_settings_result = ESP_OK;
    s_calc_settings_json =
        "{\"propagation_speed_mps\":340,\"propagation_speed_uncertainty_mps\":5,"
        "\"propagation_model_name\":\"tap_calibrated\",\"timing_uncertainty_us\":2000,"
        "\"noise_window_samples\":50,\"sta_window_samples\":5,\"lta_window_samples\":50,"
        "\"sta_lta_trigger_ratio\":3.5,\"refine_lookback_samples\":10,"
        "\"refine_lookahead_samples\":10,\"refine_amplitude_fraction\":0.5,"
        "\"snr_signal_window_samples\":50,\"minimum_snr\":2,"
        "\"semblance_window_half_span_samples\":25}";
}

esp_err_t object_store_get_calc_settings_json(const char *object_id, char **json_out)
{
    assert(object_id != NULL && json_out != NULL);
    *json_out = NULL;
    if (s_calc_settings_result != ESP_OK) return s_calc_settings_result;
    if (s_calc_settings_json == NULL) return ESP_ERR_NOT_FOUND;
    const size_t length = strlen(s_calc_settings_json) + 1U;
    *json_out = malloc(length);
    if (*json_out == NULL) return ESP_ERR_NO_MEM;
    memcpy(*json_out, s_calc_settings_json, length);
    return ESP_OK;
}

esp_err_t seismic_recorder_read_session_geometry(
    const char *object_id,
    const char *session_id,
    object_store_geometry_t *geometry
)
{
    (void)object_id;
    (void)session_id;
    assert(geometry != NULL);
    s_geometry_calls++;
    if (s_geometry_result != ESP_OK) {
        return s_geometry_result;
    }

    memset(geometry, 0, sizeof(*geometry));
    geometry->pods[0].computed = true;
    geometry->pods[0].leveled = true;
    geometry->pods[0].x_mm = 300;
    geometry->pods[0].y_mm = -150;
    geometry->pods[0].z_mm = 50;
    geometry->pods[0].computed_at_ms = 111ULL;
    geometry->pods[1].computed = true;
    geometry->pods[1].leveled = false;
    geometry->pods[1].x_mm = -300;
    geometry->pods[1].y_mm = 150;
    geometry->pods[1].z_mm = -20;
    geometry->pods[1].computed_at_ms = 222ULL;
    /* pods 2 and 3 stay not-computed, matching a session with only 3
     * participating REMOTEs. */
    return ESP_OK;
}


esp_err_t seismic_recorder_read_session_participation(
    const char *object_id,
    const char *session_id,
    seismic_recorder_session_participation_t *participation
)
{
    (void)object_id;
    (void)session_id;
    assert(participation != NULL);
    ++s_participation_calls;
    if (s_participation_result != ESP_OK) {
        return s_participation_result;
    }
    *participation = s_participation;
    return ESP_OK;
}

esp_err_t seismic_recorder_read_session_cs_pairs(
    const char *object_id,
    const char *session_id,
    object_store_cs_pairs_t *cs_pairs
)
{
    (void)object_id;
    (void)session_id;
    assert(cs_pairs != NULL);
    s_cs_pairs_calls++;
    if (s_cs_pairs_result != ESP_OK) {
        return s_cs_pairs_result;
    }

    memset(cs_pairs, 0, sizeof(*cs_pairs));
    cs_pairs->pair_count = 1U;
    cs_pairs->pairs[0].node_a = 0U;
    cs_pairs->pairs[0].node_b = 1U;
    cs_pairs->pairs[0].valid = true;
    cs_pairs->pairs[0].distance_mm = 3200;
    cs_pairs->pairs[0].confidence = 950U;
    cs_pairs->pairs[0].attempts = 1U;
    cs_pairs->pairs[0].valid_samples = 12U;
    snprintf(cs_pairs->pairs[0].state, sizeof(cs_pairs->pairs[0].state), "valid");
    snprintf(cs_pairs->pairs[0].last_failure, sizeof(cs_pairs->pairs[0].last_failure), "timeout");
    cs_pairs->pairs[0].provider_quality_flags = 3U;
    cs_pairs->pairs[0].scheduler_quality_flags = 1U;
    return ESP_OK;
}

esp_err_t localization_run_store_list_picks_ex(
    const char *object_id,
    const char *session_id,
    localization_pick_record_t *records,
    size_t max_records,
    size_t *record_count_out,
    localization_run_store_list_status_t *status_out
)
{
    (void)object_id;
    (void)session_id;
    assert(records != NULL && max_records >= 1U && record_count_out != NULL && status_out != NULL);
    s_picks_list_calls++;
    *record_count_out = 0U;
    *status_out = s_picks_list_status;
    assert(s_picks_record_count <= max_records);

    if (s_picks_record_count > 0U) {
        memset(&records[0], 0, sizeof(records[0]));
        records[0].schema_version = LOCALIZATION_PICK_RECORD_SCHEMA_VERSION;
        snprintf(records[0].object_id, sizeof(records[0].object_id), "OBJ_0001");
        snprintf(records[0].session_id, sizeof(records[0].session_id), "S000001");
        records[0].computed_at_ms = 999ULL;
        records[0].session_epoch_known = true;
        records[0].session_epoch_timestamp_us = INT64_C(1700000000123456);
        records[0].channels[0].participating = true;
        records[0].channels[0].pick.status = GEOPHONE_PICK_ACCEPTED;
        records[0].channels[0].pick.method = GEOPHONE_PICK_METHOD_STA_LTA_REFINED;
        records[0].channels[0].pick.arrival_sample_index = 1200U;
        records[0].channels[0].pick.arrival_time_s = 2.4f;
        records[0].channels[0].pick.snr = 14.5f;
        records[0].channels[0].pick.uncertainty_samples = 5U;
        records[0].channels[0].s_pick.present = true;
        records[0].channels[0].s_pick.status = GEOPHONE_PICK_ACCEPTED;
        records[0].channels[0].s_pick.method = GEOPHONE_PICK_METHOD_STA_LTA_REFINED;
        records[0].channels[0].s_pick.sample_index = 2400U;
        records[0].channels[0].s_pick.subsample_offset = -0.25f;
        records[0].channels[0].s_pick.arrival_time_us = 4800000.0;
        records[0].channels[0].s_pick.snr = 9.5;
        records[0].channels[0].s_pick.uncertainty_us = 20.0;
        records[0].channels[0].s_pick.fields_present = LOCALIZATION_PICK_PHASE_FIELDS_ALL;
        records[0].channels[1].participating = true;
        records[0].channels[1].pick.status = GEOPHONE_PICK_REJECTED_SATURATED;
        snprintf(records[0].channels[1].reason, sizeof(records[0].channels[1].reason), "saturated");
    }
    *record_count_out = s_picks_record_count;
    return s_picks_list_result;
}

bool power_control_is_shutdown_requested(void)
{
    return false;
}

esp_err_t localization_run_store_list_ex(
    const char *object_id,
    const char *session_id,
    localization_run_record_t *records,
    size_t max_records,
    size_t *record_count_out,
    localization_run_store_list_status_t *status_out
)
{
    assert(records != NULL && record_count_out != NULL && status_out != NULL && max_records >= 2U);
    s_list_calls++;
    snprintf(s_list_object_id, sizeof(s_list_object_id), "%s", object_id);
    snprintf(s_list_session_id, sizeof(s_list_session_id), "%s", session_id);

    *record_count_out = 0U;
    *status_out = s_list_status;
    assert(s_list_record_count <= max_records);

    if (s_list_record_count > 0U) {
        memset(&records[0], 0, sizeof(records[0]));
        records[0].schema_version = LOCALIZATION_RUN_SCHEMA_VERSION;
        snprintf(records[0].object_id, sizeof(records[0].object_id), "OBJ_0001");
        snprintf(records[0].session_id, sizeof(records[0].session_id), "S000001");
        records[0].computed_at_ms = 123456ULL;
        records[0].kind = LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD;
        snprintf(records[0].method_name, sizeof(records[0].method_name), "tdoa_gauss_newton");
        records[0].dimensionality = LOCALIZATION_DIMENSIONALITY_CAPABLE_3D;
        records[0].solved = true;
        records[0].source_position = (localization_point3d_t) { 1.0, 2.0, 3.0 };
        records[0].confidence_score = 0.8;
        records[0].terminal_status = LOCALIZATION_RUN_TERMINAL_STATUS_ITERATION_LIMIT;
        records[0].session_epoch_known = true;
        records[0].session_epoch_timestamp_us = INT64_C(1700000000123456);
    }

    if (s_list_record_count > 1U) {
        memset(&records[1], 0, sizeof(records[1]));
        records[1].schema_version = LOCALIZATION_RUN_SCHEMA_VERSION;
        snprintf(records[1].object_id, sizeof(records[1].object_id), "OBJ_0001");
        snprintf(records[1].session_id, sizeof(records[1].session_id), "S000001");
        records[1].computed_at_ms = 123456ULL;
        records[1].kind = LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD;
        snprintf(records[1].method_name, sizeof(records[1].method_name), "semblance");
        records[1].dimensionality = LOCALIZATION_DIMENSIONALITY_CAPABLE_3D;
        records[1].solved = false;
        snprintf(
            records[1].rejection_reason, sizeof(records[1].rejection_reason),
            "semblance search found no coherent candidate"
        );
        records[1].failure_class = LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN;
    }

    *record_count_out = s_list_record_count;
    return s_list_result;
}

static esp_err_t query_value(
    const char *query,
    const char *key,
    char *value,
    size_t value_size
)
{
    const size_t key_length = strlen(key);
    const char *cursor = query;
    while (cursor != NULL && *cursor != '\0') {
        const char *end = strchr(cursor, '&');
        const char *equals = strchr(cursor, '=');
        if (equals != NULL && (end == NULL || equals < end)) {
            const size_t candidate_length = (size_t)(equals - cursor);
            if (candidate_length == key_length && strncmp(cursor, key, key_length) == 0) {
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
    return query_value(query, key, value, value_size);
}

int httpd_req_recv(httpd_req_t *request, void *buffer, size_t length)
{
    (void)request;
    (void)buffer;
    (void)length;
    return ESP_FAIL;
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

esp_err_t httpd_resp_send_err(httpd_req_t *request, int status, const char *message)
{
    (void)message;
    if (status == 400) request->response_status = "400 Bad Request";
    else if (status == 404) request->response_status = "404 Not Found";
    else if (status == 422) request->response_status = "422 Unprocessable Entity";
    else request->response_status = "500 Internal Server Error";
    return ESP_OK;
}

esp_err_t httpd_resp_send(httpd_req_t *request, const char *buffer, ssize_t length)
{
    const size_t bytes = length == HTTPD_RESP_USE_STRLEN ? strlen(buffer) : (size_t)length;
    assert(request != NULL && request->response_body != NULL);
    assert(bytes <= request->response_capacity);
    memcpy(request->response_body, buffer, bytes);
    request->response_length = bytes;
    return ESP_OK;
}

esp_err_t httpd_resp_send_chunk(httpd_req_t *request, const char *buffer, ssize_t length)
{
    if (buffer == NULL && length == 0) return ESP_OK;
    assert(request != NULL && buffer != NULL && length > 0);
    assert(request->response_length + (size_t)length <= request->response_capacity);
    memcpy(request->response_body + request->response_length, buffer, (size_t)length);
    request->response_length += (size_t)length;
    return ESP_OK;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *uri)
{
    (void)server;
    (void)uri;
    return ESP_OK;
}

esp_err_t web_server_route_tracking_register(const char *uri, int method)
{
    (void)uri;
    (void)method;
    return ESP_OK;
}

static unsigned s_job_status_calls;
static const char *s_job_status_json =
    "{\"state\":\"running\",\"job_id\":3,\"current_method\":\"eikonal_scan_voids\",\"elapsed_ms\":4200}";

bool storage_manager_is_ready(void)
{
    return true;
}

esp_err_t localization_job_status_json(char **json_out)
{
    s_job_status_calls++;
    const size_t length = strlen(s_job_status_json) + 1U;
    *json_out = malloc(length);
    if (*json_out == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(*json_out, s_job_status_json, length);
    return ESP_OK;
}

#define static
#include "../../components/web_server/web_localization_api.c"
#undef static
#include "../../components/web_server/web_localization_storage.c"

void localization_calculate_recommendation(
    const localization_run_record_t *records,
    size_t record_count,
    const localization_pick_record_t *pick_records,
    size_t pick_record_count,
    localization_recommendation_t *recommendation_out
)
{
    (void)pick_records;
    (void)pick_record_count;
    memset(recommendation_out, 0, sizeof(*recommendation_out));
    for (size_t i = 0U; records != NULL && i < record_count; ++i) {
        if (records[i].solved && !records[i].uncertainty_too_high) {
            recommendation_out->successful = true;
            snprintf(recommendation_out->reason_summary, sizeof(recommendation_out->reason_summary),
                     "At least one method produced a usable result.");
            return;
        }
    }
}

static httpd_req_t make_request(const char *query, char *response, size_t response_capacity)
{
    return (httpd_req_t) {
        .uri = "/api/localization/runs",
        .query = query,
        .response_body = response,
        .response_capacity = response_capacity,
    };
}

static void test_runs_listing_round_trips_persisted_records(void)
{
    reset_fakes();
    char response[8192] = {0};
    httpd_req_t request = make_request("object_id=OBJ_0001&session_id=S000001", response, sizeof(response));

    assert(web_localization_runs_handler(&request) == ESP_OK);
    assert(s_list_calls == 1U);
    assert(strcmp(s_list_object_id, "OBJ_0001") == 0);
    assert(strcmp(s_list_session_id, "S000001") == 0);
    assert(strstr(response, "\"object_id\":\"OBJ_0001\"") != NULL);
    assert(strstr(response, "\"session_id\":\"S000001\"") != NULL);
    assert(strstr(response, "\"partial_error\":false,\"partial_errors\":[]") != NULL);
    assert(strstr(response, "\"runs\":[") != NULL);
    assert(strstr(response, "tdoa_gauss_newton") != NULL);
    assert(strstr(response, "semblance") != NULL);
    assert(strstr(response, "\"solved\":true") != NULL);
    assert(strstr(response, "\"solved\":false") != NULL);
    assert(strstr(response, "\"failure_class\":\"record_again\"") != NULL);
    assert(strstr(response, "\"terminal_status\":\"iteration_limit\"") != NULL);
    assert(strstr(response, "\"session_epoch_known\":true,\"session_epoch_timestamp_us\":1700000000123456") != NULL);
    assert(strstr(response, "\"terminal_status\":\"none\"") != NULL);
    assert(strstr(response, "\"session_epoch_known\":false,\"session_epoch_timestamp_us\":null") != NULL);
    assert(strstr(response, "\"successful\":true") != NULL);
    assert(strstr(response, "\"recommendation\":{\"action\":\"none\",\"reason_summary\":\"At least one method produced a usable result.\"}") != NULL);
    assert(strstr(response, "no coherent candidate") != NULL);
    assert(s_geometry_calls == 1U);
    assert(strstr(response, "\"geometry\":{\"available\":true") != NULL);
    assert(strstr(response, "\"z_policy_effective\":\"barometric_relative\",\"z_fallback_reason\":\"\"") != NULL);
    assert(strstr(response, "\"solver\":\"nrf_geometry_solver\",\"solver_version\":\"1\"") != NULL);
    assert(strstr(response, "\"topology_generation\":143") != NULL);
    assert(strstr(response, "\"cs_membership_generation\":52") != NULL);
    assert(strstr(response, "\"setup_id\":\"1000-143-52-7\"") != NULL);
    assert(strstr(response, "\"pod\":1,\"computed\":true,\"leveled\":true") != NULL);
    assert(strstr(response, "\"x_mm\":300,\"y_mm\":-150") != NULL);
    assert(strstr(response, "\"pod\":3,\"computed\":false,\"leveled\":false") != NULL);
    assert(s_participation_calls == 1U);
    assert(strstr(response, "\"assigned\":true,\"node_id\":42,\"hardware_id\":\"01abff\",\"position_source\":\"channel_sounding_baro\"") != NULL);
    assert(strstr(response, "\"source_sample_uptime_ms\":980,\"esp_receipt_uptime_ms\":990,\"height_reference_generation\":7,\"source_age_ms_at_start\":5") != NULL);
    assert(strstr(response, "\"source_age_ms_at_start\":null") != NULL);
    assert(strstr(response, "\"assigned\":true,\"node_id\":null,\"hardware_id\":null,\"position_source\":\"channel_sounding_baro\"") != NULL);
    /* geometry must appear before runs, matching the documented response shape. */
    assert(strstr(response, "\"geometry\":") < strstr(response, "\"runs\":["));

    assert(s_cs_pairs_calls == 1U);
    assert(strstr(response, "\"cs_pairs\":{\"available\":true") != NULL);
    assert(strstr(response, "\"node_a\":0,\"node_b\":1,\"valid\":true,\"distance_mm\":3200") != NULL);
    assert(strstr(response, "\"confidence\":950,\"attempts\":1,\"valid_samples\":12") != NULL);
    assert(strstr(response, "\"state\":\"valid\",\"last_failure\":\"timeout\"") != NULL);
    assert(strstr(response, "\"provider_quality_flags\":3,\"scheduler_quality_flags\":1") != NULL);
    /* cs_pairs must appear between geometry and runs. */
    assert(strstr(response, "\"geometry\":") < strstr(response, "\"cs_pairs\":"));
    assert(strstr(response, "\"cs_pairs\":") < strstr(response, "\"runs\":["));

    assert(s_picks_list_calls == 1U);
    assert(strstr(response, "\"picks\":[") != NULL);
    assert(strstr(response, "\"computed_at_ms\":999") != NULL);
    assert(strstr(response, "\"accepted\"") != NULL);
    assert(strstr(response, "\"rejected_saturated\"") != NULL);
    assert(strstr(response, "\"reason\":\"saturated\"") != NULL);
    assert(strstr(response, "\"s_pick\":{\"status\":\"accepted\",\"method\":\"sta_lta_refined\"") != NULL);
    assert(strstr(response, "\"sample_index\":2400,\"subsample_offset\":-0.25,\"arrival_time_us\":4800000") != NULL);
    assert(strstr(response, "\"uncertainty_us\":20") != NULL);
    assert(strstr(response, "\"session_epoch_known\":true,\"session_epoch_timestamp_us\":1700000000123456") != NULL);
    /* picks must appear between cs_pairs and runs. */
    assert(strstr(response, "\"cs_pairs\":") < strstr(response, "\"picks\":["));
    assert(strstr(response, "\"picks\":[") < strstr(response, "\"runs\":["));
}

static void test_runs_listing_reports_pick_listing_failure_without_failing(void)
{
    reset_fakes();
    s_geometry_result = ESP_ERR_NOT_FOUND;
    s_cs_pairs_result = ESP_ERR_NOT_FOUND;
    s_picks_list_result = ESP_FAIL;
    s_picks_list_status.partial_error = true;
    s_picks_list_status.partial_error_codes =
        LOCALIZATION_RUN_STORE_PARTIAL_ERROR_PICKS_LISTING_FAILED;
    s_picks_record_count = 0U;
    char response[8192] = {0};
    httpd_req_t request = make_request("object_id=OBJ_0001&session_id=S000001", response, sizeof(response));

    assert(web_localization_runs_handler(&request) == ESP_OK);
    assert(request.response_status == NULL);
    assert(s_geometry_calls == 1U);
    assert(s_participation_calls == 1U);
    assert(strstr(response, "\"geometry\":{\"available\":false") != NULL);
    assert(s_cs_pairs_calls == 1U);
    assert(strstr(response, "\"cs_pairs\":{\"available\":false,\"pairs\":[]}") != NULL);
    assert(s_picks_list_calls == 1U);
    assert(strstr(response, "\"picks\":[]") != NULL);
    assert(strstr(response, "\"partial_error\":true") != NULL);
    assert(strstr(response, "\"partial_errors\":[\"picks_listing_failed\"]") != NULL);
    assert(strstr(response, "\"runs\":[") != NULL);
}

static void test_runs_listing_keeps_valid_records_and_reports_partial_errors(void)
{
    reset_fakes();
    s_list_status.partial_error = true;
    s_list_status.partial_error_codes = LOCALIZATION_RUN_STORE_PARTIAL_ERROR_RUN_RECORD_READ_FAILED;
    s_list_record_count = 1U;
    s_picks_list_result = ESP_FAIL;
    s_picks_list_status.partial_error = true;
    s_picks_list_status.partial_error_codes = LOCALIZATION_RUN_STORE_PARTIAL_ERROR_PICKS_RECORD_PARSE_FAILED;
    char response[8192] = {0};
    httpd_req_t request = make_request("object_id=OBJ_0001&session_id=S000001", response, sizeof(response));

    assert(web_localization_runs_handler(&request) == ESP_OK);
    assert(request.response_status == NULL);
    assert(strstr(response, "\"partial_error\":true") != NULL);
    assert(strstr(response, "\"partial_errors\":[\"run_record_read_failed\",\"picks_record_parse_failed\"]") != NULL);
    assert(strstr(response, "\"runs\":[{\"schema_version\":7") != NULL);
    assert(strstr(response, "\"picks\":[{\"schema_version\":1") != NULL);
    assert(strstr(response, "tdoa_gauss_newton") != NULL);
    assert(strstr(response, "\"accepted\"") != NULL);
}

static void test_all_corrupt_records_return_empty_arrays_with_parse_errors(void)
{
    reset_fakes();
    s_list_record_count = 0U;
    s_list_status.partial_error = true;
    s_list_status.partial_error_codes = LOCALIZATION_RUN_STORE_PARTIAL_ERROR_RUN_RECORD_PARSE_FAILED;
    s_picks_record_count = 0U;
    s_picks_list_status.partial_error = true;
    s_picks_list_status.partial_error_codes = LOCALIZATION_RUN_STORE_PARTIAL_ERROR_PICKS_RECORD_PARSE_FAILED;
    char response[8192] = {0};
    httpd_req_t request = make_request("object_id=OBJ_0001&session_id=S000001", response, sizeof(response));

    assert(web_localization_runs_handler(&request) == ESP_OK);
    assert(request.response_status == NULL);
    assert(strstr(response, "\"partial_error\":true") != NULL);
    assert(strstr(response, "\"partial_errors\":[\"run_record_parse_failed\",\"picks_record_parse_failed\"]") != NULL);
    assert(strstr(response, "\"picks\":[],\"runs\":[]") != NULL);
}

static void test_total_run_enumeration_failure_is_reported_in_envelope(void)
{
    reset_fakes();
    s_list_record_count = 0U;
    s_list_result = ESP_FAIL;
    s_list_status.partial_error = true;
    s_list_status.partial_error_codes = LOCALIZATION_RUN_STORE_PARTIAL_ERROR_LISTING_ENUMERATION_FAILED;
    s_picks_record_count = 0U;
    s_picks_list_result = ESP_ERR_NOT_FOUND;
    char response[8192] = {0};
    httpd_req_t request = make_request("object_id=OBJ_0001&session_id=S000001", response, sizeof(response));

    assert(web_localization_runs_handler(&request) == ESP_OK);
    assert(request.response_status == NULL);
    assert(strstr(response, "\"partial_error\":true,\"partial_errors\":[\"listing_enumeration_failed\"]") != NULL);
    assert(strstr(response, "\"runs\":[]") != NULL);
}


static void test_participation_read_failure_keeps_geometry_response(void)
{
    reset_fakes();
    s_participation_result = ESP_ERR_INVALID_RESPONSE;
    char response[8192] = {0};
    httpd_req_t request = make_request("object_id=OBJ_0001&session_id=S000001", response, sizeof(response));

    assert(web_localization_runs_handler(&request) == ESP_OK);
    assert(request.response_status == NULL);
    assert(s_geometry_calls == 1U && s_participation_calls == 1U);
    assert(strstr(response, "\"geometry\":{\"available\":true") != NULL);
    assert(strstr(response, "\"x_mm\":300,\"y_mm\":-150") != NULL);
    assert(strstr(response, "\"assigned\"") == NULL);
    assert(strstr(response, "\"node_id\"") == NULL);
    assert(strstr(response, "\"hardware_id\"") == NULL);
    assert(strstr(response, "\"position_source\"") == NULL);
}

static void test_runs_listing_includes_planar_fallback_reason(void)
{
    reset_fakes();
    s_participation.z_provenance.effective_policy = SEISMIC_RECORDER_GEOMETRY_Z_PLANAR;
    snprintf(s_participation.z_provenance.fallback_reason,
        sizeof(s_participation.z_provenance.fallback_reason), "reference_generation_mismatch");
    s_participation.pods[0].position_source = SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING;
    s_participation.pods[1].position_source = SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING;
    char response[8192] = {0};
    httpd_req_t request = make_request("object_id=OBJ_0001&session_id=S000001", response, sizeof(response));
    assert(web_localization_runs_handler(&request) == ESP_OK);
    assert(strstr(response,
        "\"z_policy_effective\":\"planar\",\"z_fallback_reason\":\"reference_generation_mismatch\"") != NULL);
}


static void test_unknown_provenance_is_omitted_and_source_age_is_null(void)
{
    reset_fakes();
    s_participation.geometry_provenance.available = false;
    s_participation.pods[0].source_age_at_start_known = false;
    char response[8192] = {0};
    httpd_req_t request = make_request("object_id=OBJ_0001&session_id=S000001", response, sizeof(response));
    assert(web_localization_runs_handler(&request) == ESP_OK);
    assert(strstr(response, "\"provenance\"") == NULL);
    assert(strstr(response, "\"source_age_ms_at_start\":null") != NULL);
}

static void test_runs_listing_requires_both_query_parameters(void)
{
    reset_fakes();
    char response[512] = {0};
    httpd_req_t request = make_request("object_id=OBJ_0001", response, sizeof(response));

    assert(web_localization_runs_handler(&request) == ESP_OK);
    assert(request.response_status != NULL);
    assert(strcmp(request.response_status, "400 Bad Request") == 0);
    assert(s_list_calls == 0U);
    assert(s_geometry_calls == 0U);
    assert(s_cs_pairs_calls == 0U);
    assert(s_picks_list_calls == 0U);
}

static void test_runs_listing_surfaces_not_found_as_404(void)
{
    reset_fakes();
    s_list_result = ESP_ERR_NOT_FOUND;
    s_list_record_count = 0U;
    char response[512] = {0};
    httpd_req_t request = make_request("object_id=OBJ_0001&session_id=S000001", response, sizeof(response));

    assert(web_localization_runs_handler(&request) == ESP_OK);
    assert(request.response_status != NULL);
    assert(strcmp(request.response_status, "404 Not Found") == 0);
    assert(s_list_calls == 1U);
    assert(s_geometry_calls == 0U);
    assert(s_cs_pairs_calls == 0U);
    assert(s_picks_list_calls == 0U);
}

static void test_absent_picks_directory_is_empty_without_partial_error(void)
{
    reset_fakes();
    s_picks_list_result = ESP_ERR_NOT_FOUND;
    s_picks_record_count = 0U;
    char response[8192] = {0};
    httpd_req_t request = make_request("object_id=OBJ_0001&session_id=S000001", response, sizeof(response));

    assert(web_localization_runs_handler(&request) == ESP_OK);
    assert(request.response_status == NULL);
    assert(strstr(response, "\"partial_error\":false,\"partial_errors\":[]") != NULL);
    assert(strstr(response, "\"picks\":[]") != NULL);
}

static void test_runs_listing_surfaces_invalid_arg_as_422(void)
{
    reset_fakes();
    s_list_result = ESP_ERR_INVALID_ARG;
    char response[512] = {0};
    httpd_req_t request = make_request("object_id=OBJ_0001&session_id=S000001", response, sizeof(response));

    assert(web_localization_runs_handler(&request) == ESP_OK);
    assert(request.response_status != NULL);
    assert(strcmp(request.response_status, "422 Unprocessable Entity") == 0);
    assert(s_list_calls == 1U);
    /* Fails closed: geometry/cs_pairs/picks are never read once the run
     * listing itself failed. */
    assert(s_geometry_calls == 0U);
    assert(s_cs_pairs_calls == 0U);
    assert(s_picks_list_calls == 0U);
}

static void test_runs_listing_surfaces_invalid_size_as_422(void)
{
    reset_fakes();
    s_list_result = ESP_ERR_INVALID_SIZE;
    char response[512] = {0};
    httpd_req_t request = make_request("object_id=OBJ_0001&session_id=S000001", response, sizeof(response));

    assert(web_localization_runs_handler(&request) == ESP_OK);
    assert(request.response_status != NULL);
    assert(strcmp(request.response_status, "422 Unprocessable Entity") == 0);
    assert(s_list_calls == 1U);
}

static void test_runs_listing_surfaces_unexpected_errors_as_500(void)
{
    reset_fakes();
    s_list_result = ESP_ERR_NO_MEM;
    char response[512] = {0};
    httpd_req_t request = make_request("object_id=OBJ_0001&session_id=S000001", response, sizeof(response));

    assert(web_localization_runs_handler(&request) == ESP_OK);
    assert(request.response_status != NULL);
    assert(strcmp(request.response_status, "500 Internal Server Error") == 0);
    assert(s_list_calls == 1U);

    reset_fakes();
    /* Any other error code this store could return also fails closed as
     * 500 rather than being silently reported as a successful, empty
     * listing -- web_localization_http_status()'s default branch. */
    s_list_result = ESP_FAIL;
    char response2[512] = {0};
    httpd_req_t request2 = make_request("object_id=OBJ_0001&session_id=S000001", response2, sizeof(response2));
    assert(web_localization_runs_handler(&request2) == ESP_OK);
    assert(strcmp(request2.response_status, "500 Internal Server Error") == 0);
}

static void test_runs_listing_rejects_query_string_too_long(void)
{
    reset_fakes();
    /* A query string that does not fit the handler's fixed 160-byte buffer
     * must be rejected as a bad request, not truncated/misparsed. */
    char long_query[512];
    memset(long_query, 'a', sizeof(long_query) - 1U);
    long_query[sizeof(long_query) - 1U] = '\0';
    char response[512] = {0};
    httpd_req_t request = make_request(long_query, response, sizeof(response));

    assert(web_localization_runs_handler(&request) == ESP_OK);
    assert(request.response_status != NULL);
    assert(strcmp(request.response_status, "400 Bad Request") == 0);
    assert(s_list_calls == 0U);
}

static void test_job_status_shares_the_runs_route(void)
{
    reset_fakes();
    s_job_status_calls = 0U;
    char response[512] = {0};
    httpd_req_t request = make_request("job=status", response, sizeof(response));

    assert(web_localization_runs_handler(&request) == ESP_OK);
    assert(request.response_status == NULL);
    assert(s_job_status_calls == 1U);
    assert(strcmp(response, s_job_status_json) == 0);
    assert(request.cache_control != NULL && strcmp(request.cache_control, "no-store") == 0);
    /* No session work happens for a status poll. */
    assert(s_list_calls == 0U && s_geometry_calls == 0U && s_picks_list_calls == 0U);

    /* Any other job value is an ordinary (incomplete) runs query. */
    memset(response, 0, sizeof(response));
    request = make_request("job=start", response, sizeof(response));
    assert(web_localization_runs_handler(&request) == ESP_OK);
    assert(request.response_status != NULL && strcmp(request.response_status, "400 Bad Request") == 0);
    assert(s_job_status_calls == 1U);
}

static void test_settings_read_returns_valid_json_or_null(void)
{
    reset_fakes();
    char response[4096] = {0};
    httpd_req_t request = make_request("job=settings&object_id=OBJ_0001", response, sizeof(response));
    assert(web_localization_runs_handler(&request) == ESP_OK);
    assert(request.response_status == NULL);
    assert(strstr(response, "{\"ok\":true,\"settings\":{") == response);
    assert(strstr(response, "\"propagation_speed_mps\":340") != NULL);
    assert(strstr(response, "\"material\":\"custom\"") != NULL);
    assert(strstr(response, "\"presets\":[") != NULL);
    assert(strstr(response, "\"standard_processing\":{") != NULL);
    cJSON *body = cJSON_Parse(response);
    assert(body != NULL);
    cJSON *settings = cJSON_GetObjectItemCaseSensitive(body, "settings");
    cJSON *presets = cJSON_GetObjectItemCaseSensitive(body, "presets");
    cJSON *standard = cJSON_GetObjectItemCaseSensitive(body, "standard_processing");
    assert(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(settings, "material")));
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(settings, "material")->valuestring, "custom") == 0);
    assert(cJSON_GetArraySize(presets) == 7);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(presets, 0), "id")->valuestring,
        "concrete") == 0);
    assert(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(presets, 0),
        "propagation_speed_mps")->valuedouble == 4000.0);
    assert(cJSON_GetObjectItemCaseSensitive(standard, "timing_uncertainty_us")->valuedouble == 2000.0);
    assert(cJSON_GetObjectItemCaseSensitive(standard, "sta_lta_trigger_ratio")->valuedouble == 3.0);
    assert(cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(standard, "s_pick_enabled")));
    assert(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(standard, "s_pick_enabled")));
    assert(cJSON_GetObjectItemCaseSensitive(standard, "time_budget_s")->valuedouble == 60.0);
    cJSON_Delete(body);
    assert(request.content_type != NULL && strcmp(request.content_type, "application/json; charset=utf-8") == 0);
    assert(request.cache_control != NULL && strcmp(request.cache_control, "no-store") == 0);
    assert(s_list_calls == 0U);

    s_calc_settings_json = NULL;
    memset(response, 0, sizeof(response));
    request = make_request("job=settings&object_id=OBJ_0001", response, sizeof(response));
    assert(web_localization_runs_handler(&request) == ESP_OK);
    assert(strstr(response, "{\"ok\":true,\"settings\":null,") == response);
    assert(strstr(response, "\"presets\":[") != NULL);

    s_calc_settings_json = "{";
    memset(response, 0, sizeof(response));
    request = make_request("job=settings&object_id=OBJ_0001", response, sizeof(response));
    assert(web_localization_runs_handler(&request) == ESP_OK);
    assert(strstr(response, "{\"ok\":true,\"settings\":null,") == response);

    s_calc_settings_result = ESP_FAIL;
    memset(response, 0, sizeof(response));
    request = make_request("job=settings&object_id=OBJ_0001", response, sizeof(response));
    assert(web_localization_runs_handler(&request) == ESP_OK);
    assert(request.response_status != NULL && strcmp(request.response_status, "500 Internal Server Error") == 0);

    memset(response, 0, sizeof(response));
    request = make_request("job=settings", response, sizeof(response));
    assert(web_localization_runs_handler(&request) == ESP_OK);
    assert(request.response_status != NULL && strcmp(request.response_status, "400 Bad Request") == 0);
}

int main(void)
{
    test_runs_listing_round_trips_persisted_records();
    test_runs_listing_reports_pick_listing_failure_without_failing();
    test_runs_listing_keeps_valid_records_and_reports_partial_errors();
    test_all_corrupt_records_return_empty_arrays_with_parse_errors();
    test_total_run_enumeration_failure_is_reported_in_envelope();
    test_participation_read_failure_keeps_geometry_response();
    test_runs_listing_includes_planar_fallback_reason();
    test_unknown_provenance_is_omitted_and_source_age_is_null();
    test_runs_listing_requires_both_query_parameters();
    test_runs_listing_surfaces_not_found_as_404();
    test_absent_picks_directory_is_empty_without_partial_error();
    test_runs_listing_surfaces_invalid_arg_as_422();
    test_runs_listing_surfaces_invalid_size_as_422();
    test_runs_listing_surfaces_unexpected_errors_as_500();
    test_runs_listing_rejects_query_string_too_long();
    test_job_status_shares_the_runs_route();
    test_settings_read_returns_valid_json_or_null();
    return 0;
}
