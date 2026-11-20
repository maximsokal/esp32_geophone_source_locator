#include "web_server_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "localization_config.h"
#include "localization_job.h"
#include "localization_material_presets.h"
#include "cJSON.h"
#include "../localization_calculate/include/localization_calculate.h"
#include "localization_pick_record.h"
#include "localization_run_record.h"
#include "localization_run_store.h"
#include "object_store.h"
#include "seismic_recorder.h"
#include "storage_bulk_admission.h"
#include "storage_manager.h"
#include "web_localization_storage.h"

void localization_calculate_recommendation(
    const localization_run_record_t *records,
    size_t record_count,
    const localization_pick_record_t *pick_records,
    size_t pick_record_count,
    localization_recommendation_t *recommendation_out
);

static const char *web_localization_z_fallback_reason(const char *reason)
{
    static const char *const reasons[] = {
        "",
        "main_reference_invalid",
        "remote_reference_invalid",
        "reference_generation_mismatch",
    };
    for (size_t i = 0U; i < sizeof(reasons) / sizeof(reasons[0]); ++i) {
        if (reason != NULL && strcmp(reason, reasons[i]) == 0) return reasons[i];
    }
    return NULL;
}

static const char *web_localization_cs_pair_state(const char *state)
{
    static const char *const states[] = {
        "unknown", "pending", "valid", "missing", "retry_exhausted",
    };
    for (size_t i = 0U; i < sizeof(states) / sizeof(states[0]); ++i) {
        if (state != NULL && strcmp(state, states[i]) == 0) return states[i];
    }
    return "unknown";
}

static const char *web_localization_cs_pair_failure(const char *failure)
{
    static const char *const failures[] = {
        "unknown", "none", "timeout", "transport", "outlier", "disconnected",
    };
    for (size_t i = 0U; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        if (failure != NULL && strcmp(failure, failures[i]) == 0) return failures[i];
    }
    return "unknown";
}

static const char *web_localization_partial_error_name(uint32_t code)
{
    switch (code) {
        case LOCALIZATION_RUN_STORE_PARTIAL_ERROR_RUN_RECORD_PARSE_FAILED:
            return "run_record_parse_failed";
        case LOCALIZATION_RUN_STORE_PARTIAL_ERROR_RUN_RECORD_READ_FAILED:
            return "run_record_read_failed";
        case LOCALIZATION_RUN_STORE_PARTIAL_ERROR_PICKS_RECORD_PARSE_FAILED:
            return "picks_record_parse_failed";
        case LOCALIZATION_RUN_STORE_PARTIAL_ERROR_PICKS_LISTING_FAILED:
            return "picks_listing_failed";
        case LOCALIZATION_RUN_STORE_PARTIAL_ERROR_LISTING_ENUMERATION_FAILED:
            return "listing_enumeration_failed";
        default:
            return NULL;
    }
}

static void web_localization_write_partial_errors(web_json_writer_t *writer, uint32_t codes)
{
    static const uint32_t error_codes[] = {
        LOCALIZATION_RUN_STORE_PARTIAL_ERROR_RUN_RECORD_PARSE_FAILED,
        LOCALIZATION_RUN_STORE_PARTIAL_ERROR_RUN_RECORD_READ_FAILED,
        LOCALIZATION_RUN_STORE_PARTIAL_ERROR_PICKS_RECORD_PARSE_FAILED,
        LOCALIZATION_RUN_STORE_PARTIAL_ERROR_PICKS_LISTING_FAILED,
        LOCALIZATION_RUN_STORE_PARTIAL_ERROR_LISTING_ENUMERATION_FAILED,
    };
    bool first = true;
    web_json_writer_write(writer, "[");
    for (size_t i = 0U; i < sizeof(error_codes) / sizeof(error_codes[0]); ++i) {
        if ((codes & error_codes[i]) == 0U) {
            continue;
        }
        const char *name = web_localization_partial_error_name(error_codes[i]);
        if (name == NULL) {
            continue;
        }
        if (!first) {
            web_json_writer_write(writer, ",");
        }
        web_json_writer_printf(writer, "\"%s\"", name);
        first = false;
    }
    web_json_writer_write(writer, "]");
}

/*
 * MAX-168: read-only listing of every localization run already persisted
 * for one session (by localization_calculate_run(), via /api/control's
 * calculate_source action). Reuses localization_run_record_to_json() for
 * each entry verbatim rather than re-deriving a response schema, so this
 * route can never drift from what localization_run_store actually wrote to
 * disk.
 *
 * MAX-148: also reports this session's own frozen per-pod sensor geometry
 * (X/Y/Z, leveled), the raw CS pair evidence behind it (distance,
 * confidence, valid/attempts), and every persisted per-channel pick
 * evidence record (one per past "Calculate source" invocation) -- the
 * exact session-frozen state and per-computation evidence a run was
 * actually produced from, never the object's current live topology. All
 * Geometry and CS-pair reads are best-effort. Run and pick record listing
 * failures that can retain valid entries are exposed through `partial_error`
 * and stable `partial_errors` codes. A missing run directory and hard run
 * listing failures retain their existing HTTP status responses.
 *
 * `?job=status` instead reports the background "Calculate source" job
 * (localization_job): state, method running, elapsed / budget, every
 * method's measured time and the worker's stack high-water mark. It shares
 * this URI so the HTTPS server spends no extra handler slot on it.
 */

static esp_err_t web_localization_query_copy(
    httpd_req_t *request,
    char *query,
    size_t query_size
)
{
    const size_t query_length = httpd_req_get_url_query_len(request);
    if (query_length == 0U || query_length >= query_size) {
        return query_length == 0U ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_SIZE;
    }
    return httpd_req_get_url_query_str(request, query, query_size);
}

static esp_err_t web_localization_runs_request_options(
    httpd_req_t *request,
    char object_id[OBJECT_STORE_ID_SIZE],
    char session_id[LOCALIZATION_RUN_SESSION_ID_SIZE]
)
{
    char query[160];
    if (web_localization_query_copy(request, query, sizeof(query)) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    if (
        httpd_query_key_value(query, "object_id", object_id, OBJECT_STORE_ID_SIZE) != ESP_OK ||
        httpd_query_key_value(query, "session_id", session_id, LOCALIZATION_RUN_SESSION_ID_SIZE) != ESP_OK
    ) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static int web_localization_http_status(esp_err_t err)
{
    switch (err) {
        case ESP_ERR_NOT_FOUND:
            return 404;
        case ESP_ERR_INVALID_ARG:
        case ESP_ERR_INVALID_SIZE:
            return 422;
        case ESP_ERR_NO_MEM:
        default:
            return 500;
    }
}


static const char *web_localization_recommendation_action_name(localization_recommendation_action_t action)
{
    switch (action) {
        case LOCALIZATION_RECOMMENDATION_RECORD_AGAIN: return "record_again";
        case LOCALIZATION_RECOMMENDATION_MOVE_SENSORS: return "move_sensors";
        case LOCALIZATION_RECOMMENDATION_NONE:
        default: return "none";
    }
}

static bool web_localization_job_status_requested(httpd_req_t *request)
{
    char query[160];
    char value[16];
    return
        web_localization_query_copy(request, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "job", value, sizeof(value)) == ESP_OK &&
        strcmp(value, "status") == 0;
}

static bool web_localization_settings_requested(
    httpd_req_t *request,
    char object_id[OBJECT_STORE_ID_SIZE]
)
{
    char query[160];
    char job[16];
    return
        web_localization_query_copy(request, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "job", job, sizeof(job)) == ESP_OK &&
        strcmp(job, "settings") == 0 &&
        httpd_query_key_value(query, "object_id", object_id, OBJECT_STORE_ID_SIZE) == ESP_OK;
}

static esp_err_t web_localization_settings_handler(httpd_req_t *request, const char *object_id)
{
    char *settings = NULL;
    const esp_err_t read_err = object_store_get_calc_settings_json(object_id, &settings);
    char *resolved_settings = NULL;
    bool valid = false;
    if (read_err == ESP_OK && settings != NULL) {
        cJSON *root = cJSON_Parse(settings);
        localization_calculate_config_t config;
        uint32_t budget_ms = 0U;
        valid = root != NULL && localization_config_from_json(root, &config, &budget_ms) == ESP_OK;
        if (valid) {
            const cJSON *material = cJSON_GetObjectItemCaseSensitive(root, "material");
            const char *material_id = cJSON_IsString(material) && material->valuestring != NULL
                ? material->valuestring : "custom";
            if (budget_ms == 0U) budget_ms = LOCALIZATION_JOB_DEFAULT_BUDGET_MS;
            valid = localization_config_to_json(
                &config, material_id, budget_ms, &resolved_settings) == ESP_OK;
        }
        cJSON_Delete(root);
    }

    if (read_err != ESP_OK && read_err != ESP_ERR_NOT_FOUND) {
        free(settings);
        return httpd_resp_send_err(request, 500, "Calculation settings unavailable");
    }
    cJSON *presets_container = cJSON_CreateObject();
    if (presets_container == NULL) {
        free(settings);
        cJSON_free(resolved_settings);
        return httpd_resp_send_err(request, 500, "Out of memory");
    }
    cJSON *presets = cJSON_AddArrayToObject(presets_container, "presets");
    bool presets_ok = presets != NULL;
    for (size_t i = 0U; presets_ok && i < localization_material_preset_count(); ++i) {
        const localization_material_preset_t *preset = localization_material_preset_at(i);
        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL ||
            cJSON_AddStringToObject(entry, "id", preset->id) == NULL ||
            cJSON_AddNumberToObject(entry, "propagation_speed_mps",
                (double)preset->propagation_speed_mps) == NULL ||
            cJSON_AddNumberToObject(entry, "propagation_speed_uncertainty_mps",
                (double)preset->propagation_speed_uncertainty_mps) == NULL ||
            !cJSON_AddItemToArray(presets, entry)) {
            cJSON_Delete(entry);
            presets_ok = false;
        }
    }
    char *presets_json = presets_ok ? cJSON_PrintUnformatted(presets) : NULL;
    cJSON_Delete(presets_container);
    char *standard_json = NULL;
    const esp_err_t standard_err = localization_config_standard_processing_to_json(&standard_json);
    if (presets_json == NULL || standard_err != ESP_OK || standard_json == NULL) {
        free(settings);
        cJSON_free(resolved_settings);
        cJSON_free(presets_json);
        cJSON_free(standard_json);
        return httpd_resp_send_err(request, 500, "Out of memory");
    }

    const char *settings_value = valid ? resolved_settings : "null";
    const size_t body_size = strlen(settings_value) + strlen(presets_json) + strlen(standard_json) +
        sizeof("{\"ok\":true,\"settings\":,\"presets\":,\"standard_processing\":}");
    char *body = malloc(body_size);
    if (body == NULL) {
        free(settings);
        cJSON_free(resolved_settings);
        cJSON_free(presets_json);
        cJSON_free(standard_json);
        return httpd_resp_send_err(request, 500, "Out of memory");
    }
    snprintf(body, body_size,
        "{\"ok\":true,\"settings\":%s,\"presets\":%s,\"standard_processing\":%s}",
        settings_value, presets_json, standard_json);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
    free(body);
    free(settings);
    cJSON_free(resolved_settings);
    cJSON_free(presets_json);
    cJSON_free(standard_json);
    return err;
}

static esp_err_t web_localization_job_status_handler(httpd_req_t *request)
{
    char *json = NULL;
    if (localization_job_status_json(&json) != ESP_OK || json == NULL) {
        return httpd_resp_send_err(request, 500, "Localization job status unavailable");
    }
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_send(request, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return err;
}

/*
 * Calculation on the phone. The page downloads a finished session (`?export=meta` for the geometry and the numbers,
 * `?export=samples` for samples.bin as it lies on the card), runs the firmware's own calculation compiled to WebAssembly
 * and sends the records back with POST. Both directions share this URI so the HTTPS server spends only one more handler
 * slot (the POST) on it.
 */
#define WEB_LOCALIZATION_EXPORT_CHUNK_BYTES 16384U
#define WEB_LOCALIZATION_IMPORT_MAX_BYTES (192U * 1024U)

typedef struct
{
    FILE *file;
    uint8_t *buffer;
    size_t capacity;
    size_t read_count;
    const char *open_path;
    size_t file_size;

} web_localization_export_chunk_t;


static esp_err_t web_localization_export_open_chunk(void *context)
{
    web_localization_export_chunk_t *chunk = context;
    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) return err;
    struct stat info;
    if (stat(chunk->open_path, &info) != 0 || (info.st_mode & S_IFMT) != S_IFREG) {
        err = ESP_ERR_NOT_FOUND;
    } else {
        chunk->file = fopen(chunk->open_path, "rb");
        chunk->file_size = (size_t)info.st_size;
        if (chunk->file == NULL) err = ESP_ERR_NOT_FOUND;
    }
    storage_manager_operation_end();
    return err;
}


static esp_err_t web_localization_export_read_chunk(void *context)
{
    web_localization_export_chunk_t *chunk = context;
    chunk->read_count = 0U;
    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) return err;
    chunk->read_count = fread(chunk->buffer, 1U, chunk->capacity, chunk->file);
    if (ferror(chunk->file) != 0) err = ESP_FAIL;
    storage_manager_operation_end();
    return err;
}


static esp_err_t web_localization_export_close_chunk(void *context)
{
    web_localization_export_chunk_t *chunk = context;
    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) return err;
    if (chunk->file != NULL) (void)fclose(chunk->file);
    chunk->file = NULL;
    storage_manager_operation_end();
    return ESP_OK;
}


static bool web_localization_export_mode(httpd_req_t *request, char mode[12])
{
    char query[160];
    return web_localization_query_copy(request, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "export", mode, 12U) == ESP_OK;
}


/* Only a finished, readable session is handed out: one still being written has no stable end. */
static esp_err_t web_localization_export_summary(
    const char *object_id,
    const char *session_id,
    seismic_recorder_session_summary_t *summary
)
{
    const esp_err_t err = seismic_recorder_get_session_summary(object_id, session_id, summary);
    if (err != ESP_OK) return err;
    return summary->complete && summary->readable && summary->samples_written > 0U ? ESP_OK : ESP_ERR_INVALID_STATE;
}


/* Adds {"geometry": {"pods": [...], "anchored": ...}} under `parent`: the shape object_store_geometry_from_json reads. */
static bool web_localization_export_add_geometry(cJSON *parent, const object_store_geometry_t *geometry)
{
    cJSON *wrapper = cJSON_AddObjectToObject(parent, "geometry");
    cJSON *node = wrapper != NULL ? cJSON_AddObjectToObject(wrapper, "geometry") : NULL;
    cJSON *pods = node != NULL ? cJSON_AddArrayToObject(node, "pods") : NULL;
    if (node == NULL || pods == NULL || cJSON_AddBoolToObject(node, "anchored", geometry->anchored) == NULL) return false;
    for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        const object_store_pod_geometry_t *pod = &geometry->pods[i];
        cJSON *item = cJSON_CreateObject();
        if (item == NULL || !cJSON_AddItemToArray(pods, item) ||
            cJSON_AddBoolToObject(item, "computed", pod->computed) == NULL) {
            return false;
        }
        if (!pod->computed) continue;
        if (cJSON_AddBoolToObject(item, "leveled", pod->leveled) == NULL ||
            cJSON_AddNumberToObject(item, "x_mm", (double)pod->x_mm) == NULL ||
            cJSON_AddNumberToObject(item, "y_mm", (double)pod->y_mm) == NULL ||
            cJSON_AddNumberToObject(item, "z_mm", (double)pod->z_mm) == NULL ||
            cJSON_AddNumberToObject(item, "residual_mm", (double)pod->residual_mm) == NULL ||
            cJSON_AddNumberToObject(item, "uncertainty_mm", (double)pod->uncertainty_mm) == NULL ||
            cJSON_AddNumberToObject(item, "computed_at_ms", (double)pod->computed_at_ms) == NULL) {
            return false;
        }
    }
    return true;
}


static esp_err_t web_localization_export_meta_handler(
    httpd_req_t *request,
    const char *object_id,
    const char *session_id
)
{
    seismic_recorder_session_summary_t summary;
    const esp_err_t summary_err = web_localization_export_summary(object_id, session_id, &summary);
    if (summary_err == ESP_ERR_INVALID_STATE) return httpd_resp_send_err(request, 409, "Session is not finished");
    if (summary_err != ESP_OK) return httpd_resp_send_err(request, web_localization_http_status(summary_err), "Session unavailable");

    object_store_geometry_t geometry;
    memset(&geometry, 0, sizeof(geometry));
    seismic_recorder_localization_geometry_source_t source = SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_CHANNEL_SOUNDING;
    if (seismic_recorder_read_session_localization_geometry(object_id, session_id, &geometry, &source, NULL) != ESP_OK) {
        return httpd_resp_send_err(request, 404, "Session geometry unavailable");
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL ||
        cJSON_AddBoolToObject(root, "ok", true) == NULL ||
        cJSON_AddStringToObject(root, "object_id", object_id) == NULL ||
        cJSON_AddStringToObject(root, "session_id", session_id) == NULL ||
        cJSON_AddNumberToObject(root, "sample_rate_hz", (double)summary.sample_rate_hz) == NULL ||
        cJSON_AddNumberToObject(root, "frames", (double)summary.samples_written) == NULL ||
        cJSON_AddNumberToObject(root, "format_version", (double)summary.format_version) == NULL ||
        cJSON_AddStringToObject(root, "geometry_source",
            source == SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_APRILTAG_REGISTERED ? "apriltag"
            : source == SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_APRILTAG_LAYOUT ? "apriltag_layout"
            : "channel_sounding") == NULL ||
        !web_localization_export_add_geometry(root, &geometry)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(request, 500, "Out of memory");
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) return httpd_resp_send_err(request, 500, "Out of memory");
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return err;
}


static esp_err_t web_localization_export_samples_handler(
    httpd_req_t *request,
    const char *object_id,
    const char *session_id
)
{
    seismic_recorder_session_summary_t summary;
    const esp_err_t summary_err = web_localization_export_summary(object_id, session_id, &summary);
    if (summary_err == ESP_ERR_INVALID_STATE) return httpd_resp_send_err(request, 409, "Session is not finished");
    if (summary_err != ESP_OK) return httpd_resp_send_err(request, web_localization_http_status(summary_err), "Session unavailable");
    if (storage_bulk_admission_is_protected()) return httpd_resp_send_err(request, 503, "Recording in progress");

    char path[160];
    const int written = snprintf(path, sizeof(path), "%s/%s/sessions/%s/samples.bin",
                                 STORAGE_MANAGER_OBJECTS_PATH, object_id, session_id);
    if (written <= 0 || (size_t)written >= sizeof(path)) return httpd_resp_send_err(request, 400, "Path too long");

    web_localization_export_chunk_t chunk = {
        .buffer = malloc(WEB_LOCALIZATION_EXPORT_CHUNK_BYTES), .capacity = WEB_LOCALIZATION_EXPORT_CHUNK_BYTES,
        .open_path = path,
    };
    if (chunk.buffer == NULL) return httpd_resp_send_err(request, 500, "Out of memory");
    esp_err_t err = storage_bulk_admission_run_chunk(web_localization_export_open_chunk, &chunk);
    if (err != ESP_OK) {
        free(chunk.buffer);
        return httpd_resp_send_err(request, err == ESP_ERR_NOT_FOUND ? 404 : 503, "samples.bin unavailable");
    }

    char length[24];
    (void)snprintf(length, sizeof(length), "%u", (unsigned)chunk.file_size);
    httpd_resp_set_type(request, "application/octet-stream");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Samples-Bytes", length);
    size_t sent = 0U;
    while (err == ESP_OK && sent < chunk.file_size) {
        err = storage_bulk_admission_run_chunk(web_localization_export_read_chunk, &chunk);
        if (err != ESP_OK || chunk.read_count == 0U) {
            if (err == ESP_OK) err = ESP_FAIL;
            break;
        }
        if (httpd_resp_send_chunk(request, (const char *)chunk.buffer, (ssize_t)chunk.read_count) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
        sent += chunk.read_count;
    }
    (void)storage_bulk_admission_run_chunk(web_localization_export_close_chunk, &chunk);
    free(chunk.buffer);
    /* A short body tells the page the download failed (it compares with X-Samples-Bytes); end the stream either way. */
    return httpd_resp_send_chunk(request, NULL, 0);
}


/*
 * The result the phone calculated. The device stores it exactly as its own run would: every record is parsed by the
 * schema reader first (nothing is written if any is malformed), then written through the run store.
 */
static esp_err_t web_localization_import_handler(httpd_req_t *request)
{
    esp_err_t storage_response = ESP_OK;
    if (web_localization_runs_reject_if_storage_unavailable(request, &storage_response)) return storage_response;
    if (request->content_len == 0U || request->content_len > WEB_LOCALIZATION_IMPORT_MAX_BYTES) {
        return httpd_resp_send_err(request, 413, "Result size is not accepted");
    }
    char *body = malloc(request->content_len + 1U);
    if (body == NULL) return httpd_resp_send_err(request, 500, "Out of memory");
    size_t received = 0U;
    while (received < request->content_len) {
        const int count = httpd_req_recv(request, body + received, request->content_len - received);
        if (count <= 0) {
            free(body);
            return httpd_resp_send_err(request, 400, "Result upload interrupted");
        }
        received += (size_t)count;
    }
    body[received] = '\0';
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) return httpd_resp_send_err(request, 400, "Result is not JSON");

    const cJSON *object_item = cJSON_GetObjectItemCaseSensitive(root, "object_id");
    const cJSON *session_item = cJSON_GetObjectItemCaseSensitive(root, "session_id");
    const cJSON *runs_item = cJSON_GetObjectItemCaseSensitive(root, "runs");
    const cJSON *picks_item = cJSON_GetObjectItemCaseSensitive(root, "picks");
    char object_id[OBJECT_STORE_ID_SIZE] = {0};
    char session_id[LOCALIZATION_RUN_SESSION_ID_SIZE] = {0};
    if (!cJSON_IsString(object_item) || !cJSON_IsString(session_item) || !cJSON_IsArray(runs_item) ||
        strlen(object_item->valuestring) >= sizeof(object_id) || strlen(session_item->valuestring) >= sizeof(session_id)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(request, 422, "object_id, session_id and runs are required");
    }
    snprintf(object_id, sizeof(object_id), "%s", object_item->valuestring);
    snprintf(session_id, sizeof(session_id), "%s", session_item->valuestring);

    const int run_count = cJSON_GetArraySize(runs_item);
    if (run_count <= 0 || (size_t)run_count > LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION) {
        cJSON_Delete(root);
        return httpd_resp_send_err(request, 422, "Run count is not accepted");
    }
    seismic_recorder_session_summary_t summary;
    const esp_err_t summary_err = web_localization_export_summary(object_id, session_id, &summary);
    if (summary_err != ESP_OK) {
        cJSON_Delete(root);
        return httpd_resp_send_err(request, summary_err == ESP_ERR_INVALID_STATE ? 409 : 404, "Session is not finished or unknown");
    }
    localization_job_status_t job;
    localization_job_get_status(&job);
    if ((job.state == LOCALIZATION_JOB_QUEUED || job.state == LOCALIZATION_JOB_RUNNING) &&
        strcmp(job.object_id, object_id) == 0 && strcmp(job.session_id, session_id) == 0) {
        cJSON_Delete(root);
        return httpd_resp_send_err(request, 409, "The device is calculating this session");
    }

    localization_run_record_t *records = calloc((size_t)run_count, sizeof(*records));
    localization_pick_record_t *picks = calloc(1U, sizeof(*picks));
    if (records == NULL || picks == NULL) {
        free(records);
        free(picks);
        cJSON_Delete(root);
        return httpd_resp_send_err(request, 500, "Out of memory");
    }
    esp_err_t err = ESP_OK;
    for (int i = 0; err == ESP_OK && i < run_count; ++i) {
        char *text = cJSON_PrintUnformatted(cJSON_GetArrayItem(runs_item, i));
        err = text != NULL ? localization_run_record_from_json(text, &records[i]) : ESP_ERR_NO_MEM;
        cJSON_free(text);
        /* A record that names another object or session must never be filed under this one. */
        if (err == ESP_OK &&
            (strcmp(records[i].object_id, object_id) != 0 || strcmp(records[i].session_id, session_id) != 0)) {
            err = ESP_ERR_INVALID_ARG;
        }
    }
    const bool has_picks = cJSON_IsObject(picks_item);
    if (err == ESP_OK && has_picks) {
        char *text = cJSON_PrintUnformatted(picks_item);
        err = text != NULL ? localization_pick_record_from_json(text, picks) : ESP_ERR_NO_MEM;
        cJSON_free(text);
        if (err == ESP_OK && (strcmp(picks->object_id, object_id) != 0 || strcmp(picks->session_id, session_id) != 0)) {
            err = ESP_ERR_INVALID_ARG;
        }
    }
    cJSON_Delete(root);
    for (int i = 0; err == ESP_OK && i < run_count; ++i) {
        err = localization_run_store_write(object_id, session_id, &records[i]);
    }
    if (err == ESP_OK && has_picks) err = localization_run_store_write_picks(object_id, session_id, picks);
    free(records);
    free(picks);
    if (err != ESP_OK) {
        return httpd_resp_send_err(request, err == ESP_ERR_NO_MEM ? 500 : 422, "Result was not accepted");
    }
    char reply[96];
    (void)snprintf(reply, sizeof(reply), "{\"ok\":true,\"runs\":%d,\"picks\":%s}", run_count, has_picks ? "true" : "false");
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, reply, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t web_localization_runs_handler(httpd_req_t *request)
{
    if (web_localization_job_status_requested(request)) {
        return web_localization_job_status_handler(request);
    }

    char settings_object_id[OBJECT_STORE_ID_SIZE] = {0};
    if (web_localization_settings_requested(request, settings_object_id)) {
        return web_localization_settings_handler(request, settings_object_id);
    }

    esp_err_t storage_response = ESP_OK;
    if (web_localization_runs_reject_if_storage_unavailable(request, &storage_response)) {
        return storage_response;
    }

    char object_id[OBJECT_STORE_ID_SIZE] = {0};
    char session_id[LOCALIZATION_RUN_SESSION_ID_SIZE] = {0};

    if (web_localization_runs_request_options(request, object_id, session_id) != ESP_OK) {
        return httpd_resp_send_err(request, 400, "object_id and session_id are required");
    }

    char export_mode[12];
    if (web_localization_export_mode(request, export_mode)) {
        if (strcmp(export_mode, "meta") == 0) return web_localization_export_meta_handler(request, object_id, session_id);
        if (strcmp(export_mode, "samples") == 0) return web_localization_export_samples_handler(request, object_id, session_id);
        return httpd_resp_send_err(request, 400, "Unknown export mode");
    }

    localization_run_record_t *records = calloc(
        LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION, sizeof(*records)
    );
    if (records == NULL) {
        return httpd_resp_send_err(request, 500, "Out of memory");
    }

    size_t record_count = 0U;
    localization_run_store_list_status_t run_list_status = {0};
    const esp_err_t list_err = localization_run_store_list_ex(
        object_id, session_id, records, LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION,
        &record_count, &run_list_status
    );
    /* The extended store uses ESP_FAIL for enumeration errors after setting
     * partial_error. Retain any valid records and return them with the
     * warning envelope. Missing directories and hard failures keep their
     * existing 404/422/500 behavior. */
    if (list_err != ESP_OK && !(list_err == ESP_FAIL && run_list_status.partial_error)) {
        free(records);
        return httpd_resp_send_err(
            request,
            web_localization_http_status(list_err),
            list_err == ESP_ERR_NOT_FOUND
                ? "No localization runs for this session"
                : "Localization run listing failed"
        );
    }

    /* The layout this session's localizations worked with: the frozen one, or the registered AprilTag centres. */
    object_store_geometry_t geometry;
    seismic_recorder_localization_geometry_source_t geometry_source =
        SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_CHANNEL_SOUNDING;
    const bool geometry_available = seismic_recorder_read_session_localization_geometry(
        object_id, session_id, &geometry, &geometry_source, NULL) == ESP_OK;

    seismic_recorder_session_participation_t participation;
    const bool participation_available =
        seismic_recorder_read_session_participation(object_id, session_id, &participation) == ESP_OK;

    object_store_cs_pairs_t cs_pairs;
    const bool cs_pairs_available =
        seismic_recorder_read_session_cs_pairs(object_id, session_id, &cs_pairs) == ESP_OK;

    /* Pick evidence is best-effort. A genuinely absent picks directory
     * means no pick history; any other listing failure is made visible in
     * the envelope while successfully read entries remain available. */
    localization_pick_record_t *picks = calloc(
        LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION, sizeof(*picks)
    );
    size_t pick_count = 0U;
    localization_run_store_list_status_t picks_list_status = {0};
    uint32_t partial_error_codes = run_list_status.partial_error_codes;
    bool partial_error = run_list_status.partial_error;
    if (partial_error && partial_error_codes == 0U) {
        partial_error_codes |= LOCALIZATION_RUN_STORE_PARTIAL_ERROR_LISTING_ENUMERATION_FAILED;
    }
    if (picks == NULL) {
        partial_error = true;
        partial_error_codes |= LOCALIZATION_RUN_STORE_PARTIAL_ERROR_PICKS_LISTING_FAILED;
    } else {
        const esp_err_t picks_err = localization_run_store_list_picks_ex(
            object_id, session_id, picks, LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION,
            &pick_count, &picks_list_status
        );
        partial_error = partial_error || picks_list_status.partial_error;
        partial_error_codes |= picks_list_status.partial_error_codes;
        if (picks_list_status.partial_error && picks_list_status.partial_error_codes == 0U) {
            partial_error_codes |= LOCALIZATION_RUN_STORE_PARTIAL_ERROR_PICKS_LISTING_FAILED;
        }
        if (picks_err != ESP_OK && picks_err != ESP_ERR_NOT_FOUND) {
            partial_error = true;
            if (picks_list_status.partial_error_codes == 0U) {
                partial_error_codes |= LOCALIZATION_RUN_STORE_PARTIAL_ERROR_PICKS_LISTING_FAILED;
            }
            if (!picks_list_status.partial_error) {
                pick_count = 0U;
            }
        }
    }
    partial_error = partial_error || partial_error_codes != 0U;

    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");

    web_json_writer_t writer;
    web_json_writer_init(&writer, request);
    web_json_writer_printf(
        &writer,
        "{\"object_id\":\"%s\",\"session_id\":\"%s\",\"partial_error\":%s,\"partial_errors\":",
        object_id,
        session_id,
        partial_error ? "true" : "false"
    );
    web_localization_write_partial_errors(&writer, partial_error_codes);
    web_json_writer_write(&writer, ",");

    web_json_writer_printf(
        &writer, "\"geometry\":{\"available\":%s", geometry_available ? "true" : "false"
    );
    if (participation_available && participation.z_provenance.available) {
        const char *policy =
            participation.z_provenance.effective_policy == SEISMIC_RECORDER_GEOMETRY_Z_PLANAR
                ? "planar"
                : participation.z_provenance.effective_policy == SEISMIC_RECORDER_GEOMETRY_Z_BAROMETRIC_RELATIVE
                    ? "barometric_relative"
                    : NULL;
        const char *reason = web_localization_z_fallback_reason(
            participation.z_provenance.fallback_reason
        );
        if (policy != NULL && reason != NULL) {
            web_json_writer_printf(
                &writer,
                ",\"z_policy_effective\":\"%s\",\"z_fallback_reason\":\"%s\"",
                policy,
                reason
            );
        }
    }
    if (participation_available && participation.geometry_provenance.available) {
        const seismic_recorder_geometry_provenance_t *provenance =
            &participation.geometry_provenance;
        web_json_writer_printf(
            &writer,
            ",\"provenance\":{\"solver\":\"%s\",\"solver_version\":\"%s\",",
            provenance->solver,
            provenance->solver_version
        );
        web_json_writer_printf(
            &writer,
            "\"refined\":%s,\"refine_iterations\":%u,\"topology_generation\":%" PRIu32 ",",
            provenance->refined ? "true" : "false",
            (unsigned)provenance->refine_iterations,
            provenance->topology_generation
        );
        web_json_writer_printf(
            &writer,
            "\"cs_membership_generation\":%" PRIu32 ",\"main_height_reference_generation\":%u,",
            provenance->cs_membership_generation,
            (unsigned)provenance->main_height_reference_generation
        );
        web_json_writer_printf(&writer, "\"setup_id\":\"%s\",", provenance->setup_id);
        web_json_writer_printf(
            &writer,
            "\"start_uptime_ms\":%" PRIu64,
            provenance->start_uptime_ms
        );
        /* Only a REMOTE_ONLY layout says so; a session of the old kind reads as the MAIN-origin geometry. */
        if (provenance->anchor_known && provenance->topology_mode[0] != '\0') {
            web_json_writer_printf(
                &writer,
                ",\"topology_mode\":\"%s\",\"anchor_node_id\":%u",
                provenance->topology_mode,
                (unsigned)provenance->anchor_node_id
            );
        }
        web_json_writer_write(&writer, "}");
    }
    web_json_writer_write(&writer, ",\"pods\":[");
    for (size_t pod = 0U; pod < OBJECT_STORE_POD_COUNT; ++pod) {
        if (pod > 0U) {
            web_json_writer_write(&writer, ",");
        }
        if (geometry_available) {
            /* Split across two printf calls: the worst-case single string
             * (large negative mm values + a full uint64 timestamp) can
             * exceed web_json_writer_printf()'s internal 128-byte buffer. */
            web_json_writer_printf(
                &writer,
                "{\"pod\":%u,\"computed\":%s,\"leveled\":%s,"
                "\"x_mm\":%" PRId32 ",\"y_mm\":%" PRId32 ",",
                (unsigned)(pod + 1U),
                geometry.pods[pod].computed ? "true" : "false",
                geometry.pods[pod].leveled ? "true" : "false",
                geometry.pods[pod].x_mm,
                geometry.pods[pod].y_mm
            );
            web_json_writer_printf(
                &writer,
                "\"z_mm\":%" PRId32 ",\"residual_mm\":%" PRId32 ",",
                geometry.pods[pod].z_mm,
                geometry.pods[pod].residual_mm
            );
            web_json_writer_printf(
                &writer,
                "\"uncertainty_mm\":%" PRId32 ",",
                geometry.pods[pod].uncertainty_mm
            );
            web_json_writer_printf(
                &writer,
                "\"computed_at_ms\":%" PRIu64,
                geometry.pods[pod].computed_at_ms
            );
        } else {
            web_json_writer_printf(
                &writer, "{\"pod\":%u,\"computed\":false,\"leveled\":false", (unsigned)(pod + 1U)
            );
        }

        if (participation_available) {
            static const char hex[] = "0123456789abcdef";
            const seismic_recorder_pod_participation_t *pod_participation =
                &participation.pods[pod];
            /* A pod placed from the registered tag centres says so, whatever the recording's own metadata recorded. */
            const bool registered_pod = geometry_available && geometry.anchored && geometry.pods[pod].computed &&
                geometry_source == SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_APRILTAG_REGISTERED;
            /* Placed from the phone's tag layout (the Channel Sounding/barometer values stay stored beside it, untouched). */
            const bool layout_pod = geometry_available && geometry.pods[pod].computed &&
                geometry_source == SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_APRILTAG_LAYOUT;
            const size_t hardware_id_length =
                pod_participation->hardware_id_length > SEISMIC_RECORDER_HARDWARE_ID_MAX
                    ? SEISMIC_RECORDER_HARDWARE_ID_MAX
                    : pod_participation->hardware_id_length;
            char hardware_id[SEISMIC_RECORDER_HARDWARE_ID_MAX * 2U + 1U];
            for (size_t byte = 0U; byte < hardware_id_length; ++byte) {
                hardware_id[byte * 2U] = hex[pod_participation->hardware_id[byte] >> 4U];
                hardware_id[byte * 2U + 1U] = hex[pod_participation->hardware_id[byte] & 0x0FU];
            }
            hardware_id[hardware_id_length * 2U] = '\0';

            web_json_writer_printf(
                &writer,
                ",\"assigned\":%s,\"node_id\":",
                pod_participation->assigned ? "true" : "false"
            );
            if (pod_participation->node_id_known) {
                web_json_writer_printf(&writer, "%u", (unsigned)pod_participation->node_id);
            } else {
                web_json_writer_write(&writer, "null");
            }
            web_json_writer_write(&writer, ",\"hardware_id\":");
            if (hardware_id_length > 0U) {
                web_json_writer_printf(&writer, "\"%s\"", hardware_id);
            } else {
                web_json_writer_write(&writer, "null");
            }
            web_json_writer_printf(
                &writer,
                ",\"position_source\":\"%s\"",
                registered_pod
                    ? "apriltag_center_approx"
                    : layout_pod
                    ? "apriltag_layout"
                    : pod_participation->position_source == SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING
                        ? "channel_sounding"
                        : pod_participation->position_source == SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING_BARO
                            ? "channel_sounding_baro"
                            : pod_participation->position_source == SEISMIC_RECORDER_POSITION_SOURCE_APRILTAG_CENTER_APPROX
                                ? "apriltag_center_approx"
                                : "none"
            );
            if (registered_pod) {
                web_json_writer_printf(&writer, ",\"tag_id\":%u", (unsigned)(pod + 1U)); /* G<k> is tag k */
            } else if (pod_participation->tag_id_known) {
                web_json_writer_printf(&writer, ",\"tag_id\":%u", (unsigned)pod_participation->tag_id);
            }
            web_json_writer_printf(
                &writer,
                ",\"source_sample_uptime_ms\":%" PRIu64 ","
                "\"esp_receipt_uptime_ms\":%" PRIu64 ","
                "\"height_reference_generation\":%u,\"source_age_ms_at_start\":",
                pod_participation->source_sample_uptime_ms,
                pod_participation->esp_receipt_uptime_ms,
                (unsigned)pod_participation->height_reference_generation
            );
            if (pod_participation->source_age_at_start_known) {
                web_json_writer_printf(
                    &writer,
                    "%" PRIu64,
                    pod_participation->source_age_ms_at_start
                );
            } else {
                web_json_writer_write(&writer, "null");
            }
        }
        web_json_writer_write(&writer, "}");
    }
    web_json_writer_write(&writer, "]},");

    /* What was doubtful when the recording started: shown to the operator, never blocking; unavailable for older sessions. */
    const bool diagnostic_available = participation_available && participation.diagnostic.available;
    web_json_writer_printf(&writer, "\"diagnostic\":{\"available\":%s", diagnostic_available ? "true" : "false");
    if (diagnostic_available) {
        const seismic_recorder_diagnostic_metadata_t *diagnostic = &participation.diagnostic;
        web_json_writer_printf(
            &writer, ",\"gate_bypassed\":%s,\"partial_geometry\":%s,\"excluded_pairs\":[",
            participation.recording_gate.available && participation.recording_gate.gate_bypassed ? "true" : "false",
            diagnostic->partial_geometry ? "true" : "false"
        );
        for (size_t i = 0U; i < diagnostic->excluded_pair_count &&
             i < SEISMIC_RECORDER_DIAGNOSTIC_MAX_EXCLUDED_PAIRS; ++i) {
            web_json_writer_printf(
                &writer, "%s\"%u-%u\"", i > 0U ? "," : "",
                (unsigned)diagnostic->excluded_pairs[i][0], (unsigned)diagnostic->excluded_pairs[i][1]
            );
        }
        web_json_writer_printf(
            &writer, "],\"source_suspicious\":%s,\"source_unhealthy\":%s,\"source_fault_at_stop\":%s",
            diagnostic->source_suspicious ? "true" : "false", diagnostic->source_unhealthy ? "true" : "false",
            diagnostic->source_fault_at_stop ? "true" : "false"
        );
    }
    web_json_writer_write(&writer, "},");

    /*
     * MAX-148: CS pair placement-confidence evidence frozen alongside the
     * geometry above -- same best-effort contract (cs_pairs_available ==
     * false for a session recorded before this existed, never a hard
     * failure of the whole response).
     */
    web_json_writer_printf(
        &writer, "\"cs_pairs\":{\"available\":%s,\"pairs\":[", cs_pairs_available ? "true" : "false"
    );
    const size_t cs_pair_count = cs_pairs_available ? cs_pairs.pair_count : 0U;
    for (size_t i = 0U; i < cs_pair_count; ++i) {
        if (i > 0U) {
            web_json_writer_write(&writer, ",");
        }
        const object_store_cs_pair_t *pair = &cs_pairs.pairs[i];
        web_json_writer_printf(
            &writer,
            "{\"node_a\":%u,\"node_b\":%u,\"valid\":%s,\"distance_mm\":%" PRId32 ",",
            (unsigned)pair->node_a,
            (unsigned)pair->node_b,
            pair->valid ? "true" : "false",
            pair->distance_mm
        );
        web_json_writer_printf(
            &writer,
            "\"confidence\":%u,\"attempts\":%u,\"valid_samples\":%u,",
            (unsigned)pair->confidence,
            (unsigned)pair->attempts,
            (unsigned)pair->valid_samples
        );
        web_json_writer_printf(
            &writer,
            "\"spread_mm\":%" PRId32 ",\"rejected_samples\":%u,",
            pair->spread_mm,
            (unsigned)pair->rejected_samples
        );
        /* Append scheduler state and provider/scheduler quality evidence. */
        web_json_writer_printf(
            &writer,
            "\"state\":\"%s\",\"last_failure\":\"%s\",",
            web_localization_cs_pair_state(pair->state),
            web_localization_cs_pair_failure(pair->last_failure)
        );
        web_json_writer_printf(
            &writer,
            "\"provider_quality_flags\":%u,\"scheduler_quality_flags\":%u}",
            (unsigned)pair->provider_quality_flags,
            (unsigned)pair->scheduler_quality_flags
        );
    }
    web_json_writer_write(&writer, "]},");

    /* Record serializers own the per-run and per-pick wire shape. */
    web_json_writer_write(&writer, "\"picks\":[");
    for (size_t i = 0U; i < pick_count; ++i) {
        if (i > 0U) {
            web_json_writer_write(&writer, ",");
        }

        char *pick_json = NULL;
        if (localization_pick_record_to_json(&picks[i], &pick_json) == ESP_OK && pick_json != NULL) {
            web_json_writer_write(&writer, pick_json);
        } else {
            web_json_writer_write(&writer, "null");
        }
        free(pick_json);
    }
    web_json_writer_write(&writer, "],");

    web_json_writer_write(&writer, "\"runs\":[");
    for (size_t i = 0U; i < record_count; ++i) {
        if (i > 0U) {
            web_json_writer_write(&writer, ",");
        }

        char *record_json = NULL;
        if (localization_run_record_to_json(&records[i], &record_json) == ESP_OK && record_json != NULL) {
            web_json_writer_write(&writer, record_json);
        } else {
            web_json_writer_write(&writer, "null");
        }
        free(record_json);
    }

    localization_recommendation_t recommendation;
    localization_calculate_recommendation(
        records, record_count, picks, pick_count, &recommendation
    );
    web_json_writer_printf(
        &writer,
        "],\"successful\":%s,\"recommendation\":{\"action\":\"%s\",\"reason_summary\":\"%s\"}}",
        recommendation.successful ? "true" : "false",
        web_localization_recommendation_action_name(recommendation.action),
        recommendation.reason_summary
    );
    free(picks);
    free(records);
    return web_json_writer_finish(&writer);
}

esp_err_t web_localization_api_register(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const httpd_uri_t uri = {
        .uri = "/api/localization/runs",
        .method = HTTP_GET,
        .handler = web_localization_runs_handler,
        .user_ctx = NULL,
    };
    esp_err_t err = web_server_register_uri_handler(server, &uri);
    if (err != ESP_OK) return err;
    const httpd_uri_t import_uri = {
        .uri = "/api/localization/runs",
        .method = HTTP_POST,
        .handler = web_localization_import_handler,
        .user_ctx = NULL,
    };
    return web_server_register_uri_handler(server, &import_uri);
}
