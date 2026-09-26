#include "web_server_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* MSVC's <sys/stat.h> has S_IFREG but not the POSIX test macro. */
#if !defined(S_ISREG) && defined(S_IFMT) && defined(S_IFREG)
#define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
#endif

#include "app_task_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "object_format.h"
#include "object_store.h"
#include "seismic_recorder.h"
#include "storage_bulk_admission.h"
#include "storage_manager.h"

#define WEB_SCAN_QUERY_SIZE       160U
#define WEB_SCAN_PATH_SIZE        192U
#define WEB_SCAN_IO_BUFFER_SIZE   4096U
#define WEB_SCAN_MAX_BYTES        (8U * 1024U * 1024U)
#define WEB_REGISTRATION_MAX_BYTES 16384U
#define WEB_SCAN_PREVIEW_MAX_BYTES 65536U
#define WEB_SCAN_RECV_TIMEOUTS    5U
#define WEB_SCAN_DRAIN_BUFFER_SIZE 256U

static const char *TAG = "WEB_SCAN_STORE";
/*
 * MAX-162: the same session directory also holds the compact FREE/OCCUPIED
 * occupancy artifact derived from that scan (components/scan_occupancy),
 * selected with `kind=occupancy`, and a bounded surface-voxel display preview
 * selected with `kind=preview`. Both use the same crash-safe temp/backup
 * replace path and bulk admission gate as the scan.
 */
typedef struct
{
    const char *file_name;
    size_t max_bytes;
    const char *label;

} web_scan_kind_t;

static const web_scan_kind_t WEB_SCAN_KIND_SCAN = {
    .file_name = "scan-v1.bin",
    .max_bytes = WEB_SCAN_MAX_BYTES,
    .label = "3D scan",
};

static const web_scan_kind_t WEB_SCAN_KIND_OCCUPANCY = {
    .file_name = "occupancy-v1.bin",
    .max_bytes = 1024U * 1024U,
    .label = "scan occupancy",
};

static const web_scan_kind_t WEB_SCAN_KIND_PREVIEW = {
    .file_name = "preview-v1.bin",
    .max_bytes = WEB_SCAN_PREVIEW_MAX_BYTES,
    .label = "voxel scan preview",
};

static const web_scan_kind_t WEB_SCAN_KIND_REGISTRATION = {
    .file_name = "registration-v1.json",
    .max_bytes = WEB_REGISTRATION_MAX_BYTES,
    .label = "AprilTag registration",
};

static bool web_scan_kind_size_valid(const web_scan_kind_t *kind, long size)
{
    return kind != NULL && size > 0L && (unsigned long)size <= kind->max_bytes;
}


typedef struct
{
    UBaseType_t original_priority;
    bool changed;

} web_scan_priority_guard_t;


static web_scan_priority_guard_t web_scan_priority_enter(void)
{
    web_scan_priority_guard_t guard = {
        .original_priority = uxTaskPriorityGet(NULL),
        .changed = false,
    };

    /* HTTPD defaults to priority 5 while LVGL is priority 4. During a large
     * scan transfer that can let immediately-ready socket chunks reacquire the
     * storage/TFT guard before LVGL gets scheduled. Lower only this long-lived
     * handler to the normal service priority; TLS handshakes and small HTTP
     * requests keep their normal server priority. */
    if (guard.original_priority > APP_TASK_PRIORITY_STORAGE_SERVICE) {
        vTaskPrioritySet(NULL, APP_TASK_PRIORITY_STORAGE_SERVICE);
        guard.changed = true;
    }

    return guard;
}


static void web_scan_priority_leave(web_scan_priority_guard_t guard)
{
    if (guard.changed) {
        vTaskPrioritySet(NULL, guard.original_priority);
    }
}


static esp_err_t web_scan_send_json(
    httpd_req_t *request,
    const char *action,
    const char *object_id,
    const char *session_id,
    esp_err_t result,
    size_t bytes
)
{
    char body[256];
    int written = 0;
    if (result == ESP_OK) {
        written = snprintf(
            body,
            sizeof(body),
            "{\"ok\":true,\"action\":\"%s\",\"object_id\":\"%s\",\"session_id\":\"%s\",\"bytes\":%u}",
            action,
            object_id,
            session_id,
            (unsigned)bytes
        );
    }
    else {
        written = snprintf(
            body,
            sizeof(body),
            "{\"ok\":false,\"action\":\"%s\",\"object_id\":\"%s\",\"session_id\":\"%s\",\"error\":\"%s\"}",
            action,
            object_id,
            session_id,
            esp_err_to_name(result)
        );
    }
    if (written <= 0 || (size_t)written >= sizeof(body)) return ESP_ERR_INVALID_SIZE;

    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (result == ESP_ERR_INVALID_ARG || result == ESP_ERR_INVALID_SIZE || result == ESP_ERR_NOT_SUPPORTED) {
        httpd_resp_set_status(request, "400 Bad Request");
    }
    else if (result == ESP_ERR_NOT_FOUND) {
        httpd_resp_set_status(request, "404 Not Found");
    }
    else if (result == ESP_ERR_INVALID_STATE || result == ESP_ERR_TIMEOUT) {
        httpd_resp_set_status(request, "409 Conflict");
    }
    else if (result != ESP_OK) {
        httpd_resp_set_status(request, "500 Internal Server Error");
    }
    return httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
}


static esp_err_t web_scan_query_value(
    httpd_req_t *request,
    const char *key,
    char *value,
    size_t value_size
)
{
    if (request == NULL || key == NULL || value == NULL || value_size == 0U) return ESP_ERR_INVALID_ARG;
    const size_t query_length = httpd_req_get_url_query_len(request);
    if (query_length == 0U) return ESP_ERR_NOT_FOUND;
    if (query_length >= WEB_SCAN_QUERY_SIZE) return ESP_ERR_INVALID_SIZE;

    char query[WEB_SCAN_QUERY_SIZE];
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK) return ESP_ERR_INVALID_ARG;
    return httpd_query_key_value(query, key, value, value_size) == ESP_OK
        ? ESP_OK
        : ESP_ERR_NOT_FOUND;
}


static esp_err_t web_scan_paths(
    const char *object_id,
    const char *session_id,
    const web_scan_kind_t *kind,
    char final_path[WEB_SCAN_PATH_SIZE],
    char temp_path[WEB_SCAN_PATH_SIZE],
    char backup_path[WEB_SCAN_PATH_SIZE]
)
{
    if (!object_format_id_is_valid(object_id)) return ESP_ERR_INVALID_ARG;
    /* MAX-167: a scan belongs to one recording/session, under that session's
     * own directory -- never directly under the object. */
    const int final_written = snprintf(
        final_path,
        WEB_SCAN_PATH_SIZE,
        "%s/%s/sessions/%s/%s",
        STORAGE_MANAGER_OBJECTS_PATH,
        object_id,
        session_id,
        kind->file_name
    );
    if (final_written <= 0 || final_written >= (int)WEB_SCAN_PATH_SIZE) return ESP_ERR_INVALID_SIZE;
    const int temp_written = snprintf(temp_path, WEB_SCAN_PATH_SIZE, "%s.tmp", final_path);
    const int backup_written = snprintf(backup_path, WEB_SCAN_PATH_SIZE, "%s.bak", final_path);
    if (
        temp_written <= 0 || backup_written <= 0 ||
        temp_written >= (int)WEB_SCAN_PATH_SIZE || backup_written >= (int)WEB_SCAN_PATH_SIZE
    ) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}


static bool web_scan_file_exists(const char *path)
{
    struct stat info;
    return path != NULL && stat(path, &info) == 0 && S_ISREG(info.st_mode);
}


static esp_err_t web_scan_verify_object(const char *object_id)
{
    if (!object_format_id_is_valid(object_id)) return ESP_ERR_INVALID_ARG;
    object_store_profile_t profile;
    return object_store_get_profile(object_id, &profile);
}


/*
 * MAX-167: a scan may only attach to a session that actually exists for this
 * object -- this is also the request's security boundary against a
 * malformed/malicious session_id, since a caller-supplied string that does
 * not exactly match a real recorded session is rejected here, before it is
 * ever used to build a filesystem path.
 */
static esp_err_t web_scan_verify_session(const char *object_id, const char *session_id)
{
    seismic_recorder_session_summary_t sessions[SEISMIC_RECORDER_MAX_SESSIONS];
    size_t session_count = 0U;
    const esp_err_t err = seismic_recorder_list_sessions(
        object_id, sessions, SEISMIC_RECORDER_MAX_SESSIONS, &session_count);
    if (err != ESP_OK) return err;

    for (size_t i = 0U; i < session_count; ++i) {
        if (strcmp(sessions[i].session_id, session_id) == 0) return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

static bool web_scan_registration_session_is_recording(const char *object_id, const char *session_id)
{
    seismic_recorder_snapshot_t snapshot;
    if (seismic_recorder_get_snapshot(&snapshot) != ESP_OK || !snapshot.running) return false;
    return strcmp(snapshot.object_id, object_id) == 0 && strcmp(snapshot.session_id, session_id) == 0;
}

static esp_err_t web_scan_send_registration_conflict(httpd_req_t *request)
{
    static const char body[] = "{\"ok\":false,\"error\":\"registration cannot be modified while this session is recording\"}";
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_status(request, "409 Conflict");
    return httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
}


static bool web_scan_kind_uses_bulk_admission(const web_scan_kind_t *kind)
{
    return kind == &WEB_SCAN_KIND_SCAN || kind == &WEB_SCAN_KIND_OCCUPANCY || kind == &WEB_SCAN_KIND_PREVIEW;
}


static esp_err_t web_scan_send_recording_busy(
    httpd_req_t *request,
    const char *action
)
{
    char body[112];
    const int written = snprintf(
        body,
        sizeof(body),
        "{\"ok\":false,\"action\":\"%s\",\"busy_reason\":\"recording_active\"}",
        action
    );
    if (written <= 0 || (size_t)written >= sizeof(body)) return ESP_ERR_INVALID_SIZE;

    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_status(request, "409 Conflict");
    return httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
}


/* Keep a rejected scan upload from leaving unread bytes on a reusable socket. */
static void web_scan_drain_request_body(
    httpd_req_t *request,
    size_t already_received,
    size_t max_bytes
)
{
    if (
        request == NULL || request->content_len <= 0 ||
        (unsigned long)request->content_len > max_bytes
    ) {
        return;
    }

    size_t remaining = (size_t)request->content_len;
    if (already_received >= remaining) return;
    remaining -= already_received;

    char buffer[WEB_SCAN_DRAIN_BUFFER_SIZE];
    unsigned timeout_count = 0U;
    while (remaining > 0U) {
        const size_t request_size = remaining < sizeof(buffer)
            ? remaining
            : sizeof(buffer);
        const int received = httpd_req_recv(request, buffer, request_size);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeout_count >= WEB_SCAN_RECV_TIMEOUTS) return;
            continue;
        }
        if (received <= 0) return;
        timeout_count = 0U;
        remaining -= (size_t)received;
    }
}


/* storage_manager_operation_begin() must be held by the caller. */
static esp_err_t web_scan_recover_file(
    const char *final_path,
    const char *temp_path,
    const char *backup_path
)
{
    if (web_scan_file_exists(final_path)) {
        (void)remove(temp_path);
        (void)remove(backup_path);
        return ESP_OK;
    }
    (void)remove(temp_path);
    if (!web_scan_file_exists(backup_path)) return ESP_ERR_NOT_FOUND;
    return rename(backup_path, final_path) == 0 ? ESP_OK : ESP_FAIL;
}


/*
 * Keep network waits outside the storage/SPI2 arbitration window. Holding the
 * storage operation while httpd_req_recv()/send waits for Wi-Fi also keeps
 * LVGL quiesced because TFT and SD share SPI2. Every helper below therefore
 * owns only the actual FATFS operation, never socket I/O.
 */
static esp_err_t web_scan_open_temp(
    const char *temp_path,
    FILE **file_out
)
{
    if (temp_path == NULL || file_out == NULL) return ESP_ERR_INVALID_ARG;
    *file_out = NULL;

    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) return err;

    (void)remove(temp_path);
    FILE *file = fopen(temp_path, "wb");
    if (file == NULL) err = ESP_FAIL;

    storage_manager_operation_end();
    if (err == ESP_OK) *file_out = file;
    return err;
}


typedef struct
{
    FILE *file;
    const unsigned char *buffer;
    size_t size;
    bool invoked;

} web_scan_write_chunk_context_t;


static esp_err_t web_scan_write_chunk(void *context)
{
    web_scan_write_chunk_context_t *chunk = context;
    if (chunk == NULL) return ESP_ERR_INVALID_ARG;
    chunk->invoked = true;
    if (chunk->file == NULL || chunk->buffer == NULL || chunk->size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) return err;

    if (fwrite(chunk->buffer, 1U, chunk->size, chunk->file) != chunk->size) {
        err = ESP_FAIL;
    }

    storage_manager_operation_end();
    return err;
}


static esp_err_t web_scan_close_writer(FILE **file)
{
    if (file == NULL || *file == NULL) return ESP_OK;

    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        /* Match recorder shutdown semantics: never leak a libc FILE handle if
         * storage has already become unavailable. This path is exceptional;
         * normal close/flush always runs under the SPI2 guard. */
        const int forced_close = fclose(*file);
        *file = NULL;
        ESP_LOGW(TAG, "Forced scan writer close without storage guard: lock=%s fclose=%d",
                 esp_err_to_name(err), forced_close);
        return err;
    }

    const int flush_result = fflush(*file);
    const int close_result = fclose(*file);
    *file = NULL;
    storage_manager_operation_end();

    return flush_result == 0 && close_result == 0
        ? ESP_OK
        : ESP_FAIL;
}


static esp_err_t web_scan_close_reader(FILE **file)
{
    if (file == NULL || *file == NULL) return ESP_OK;

    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        const int forced_close = fclose(*file);
        *file = NULL;
        ESP_LOGW(TAG, "Forced scan reader close without storage guard: lock=%s fclose=%d",
                 esp_err_to_name(err), forced_close);
        return err;
    }

    const int close_result = fclose(*file);
    *file = NULL;
    storage_manager_operation_end();
    return close_result == 0 ? ESP_OK : ESP_FAIL;
}


static esp_err_t web_scan_commit_temp(
    const char *final_path,
    const char *temp_path,
    const char *backup_path
)
{
    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) return err;

    (void)remove(backup_path);
    const bool had_final = web_scan_file_exists(final_path);
    if (had_final && rename(final_path, backup_path) != 0) {
        err = ESP_FAIL;
    }
    else if (rename(temp_path, final_path) != 0) {
        if (had_final) (void)rename(backup_path, final_path);
        err = ESP_FAIL;
    }
    else {
        (void)remove(backup_path);
    }

    storage_manager_operation_end();
    return err;
}


static void web_scan_remove_temp_best_effort(const char *temp_path)
{
    if (storage_manager_operation_begin() != ESP_OK) return;
    (void)remove(temp_path);
    storage_manager_operation_end();
}


static esp_err_t web_scan_save(httpd_req_t *request, const char *object_id, const char *session_id, const web_scan_kind_t *kind)
{
    if (
        web_scan_kind_uses_bulk_admission(kind) &&
        storage_bulk_admission_is_protected()
    ) {
        web_scan_drain_request_body(request, 0U, kind->max_bytes);
        return web_scan_send_recording_busy(request, "scan_save");
    }

    if (!web_scan_kind_size_valid(kind, request->content_len)) {
        return web_scan_send_json(request, "scan_save", object_id, session_id, ESP_ERR_INVALID_SIZE, 0U);
    }

    esp_err_t err = web_scan_verify_object(object_id);
    if (err == ESP_OK) err = web_scan_verify_session(object_id, session_id);
    if (err != ESP_OK) return web_scan_send_json(request, "scan_save", object_id, session_id, err, 0U);
    if (kind == &WEB_SCAN_KIND_REGISTRATION && web_scan_registration_session_is_recording(object_id, session_id)) {
        return web_scan_send_registration_conflict(request);
    }

    char final_path[WEB_SCAN_PATH_SIZE], temp_path[WEB_SCAN_PATH_SIZE], backup_path[WEB_SCAN_PATH_SIZE];
    err = web_scan_paths(object_id, session_id, kind, final_path, temp_path, backup_path);
    if (err != ESP_OK) return web_scan_send_json(request, "scan_save", object_id, session_id, err, 0U);

    const int64_t started_us = esp_timer_get_time();
    unsigned char *buffer = heap_caps_malloc(
        WEB_SCAN_IO_BUFFER_SIZE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (buffer == NULL) {
        return web_scan_send_json(request, "scan_save", object_id, session_id, ESP_ERR_NO_MEM, 0U);
    }

    const web_scan_priority_guard_t priority_guard = web_scan_priority_enter();
    FILE *file = NULL;
    size_t received_total = 0U;
    size_t request_bytes_received = 0U;
    unsigned timeout_count = 0U;
    bool admission_rejected = false;

    err = web_scan_open_temp(temp_path, &file);
    if (err == ESP_OK) {
        while (received_total < (size_t)request->content_len) {
            if (web_server_shutdown_requested()) {
                err = ESP_ERR_INVALID_STATE;
                break;
            }

            const size_t remaining = (size_t)request->content_len - received_total;
            const size_t request_size = remaining < WEB_SCAN_IO_BUFFER_SIZE
                ? remaining
                : WEB_SCAN_IO_BUFFER_SIZE;

            /* Network receive is deliberately outside storage_manager. */
            const int received = httpd_req_recv(
                request,
                (char *)buffer,
                request_size
            );
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                if (++timeout_count >= WEB_SCAN_RECV_TIMEOUTS) {
                    err = ESP_ERR_TIMEOUT;
                    break;
                }
                continue;
            }
            if (received <= 0) {
                err = ESP_FAIL;
                break;
            }
            request_bytes_received += (size_t)received;

            /* The latch may become true while recv() is blocked. Never write
             * even the final received chunk after the shutdown boundary. */
            if (web_server_shutdown_requested()) {
                err = ESP_ERR_INVALID_STATE;
                break;
            }

            timeout_count = 0U;
            web_scan_write_chunk_context_t chunk = {
                .file = file,
                .buffer = buffer,
                .size = (size_t)received,
                .invoked = false,
            };
            err = web_scan_kind_uses_bulk_admission(kind)
                ? storage_bulk_admission_run_chunk(
                    web_scan_write_chunk,
                    &chunk
                )
                : web_scan_write_chunk(&chunk);
            if (err == ESP_ERR_INVALID_STATE && !chunk.invoked) {
                admission_rejected = true;
            }
            if (err != ESP_OK) break;
            received_total += (size_t)received;
        }
    }

    if (file != NULL) {
        const esp_err_t close_err = web_scan_close_writer(&file);
        if (err == ESP_OK) err = close_err;
    }

    if (err == ESP_OK) {
        /* A scan whose final chunk was accepted before the latch is already
         * in-flight work. Finish its existing temp-file commit; do not turn the
         * shutdown admission boundary into a transactional rollback boundary. */
        err = web_scan_commit_temp(final_path, temp_path, backup_path);
    }
    else {
        web_scan_remove_temp_best_effort(temp_path);
    }

    web_scan_priority_leave(priority_guard);
    free(buffer);

    if (admission_rejected) {
        web_scan_drain_request_body(request, request_bytes_received, kind->max_bytes);
    }

    const uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - started_us) / 1000);
    if (err == ESP_OK) {
        ESP_LOGI(
            TAG,
            "Saved 3D scan: object=%s bytes=%u elapsed=%u ms chunked_storage_windows=1",
            object_id,
            (unsigned)received_total,
            (unsigned)elapsed_ms
        );
    }
    else {
        ESP_LOGE(
            TAG,
            "3D scan save failed: object=%s received=%u/%u err=%s",
            object_id,
            (unsigned)received_total,
            (unsigned)request->content_len,
            esp_err_to_name(err)
        );
    }
    return admission_rejected
        ? web_scan_send_recording_busy(request, "scan_save")
        : web_scan_send_json(request, "scan_save", object_id, session_id, err, received_total);
}


static esp_err_t web_scan_open_for_stream(
    const char *final_path,
    const char *temp_path,
    const char *backup_path,
    const web_scan_kind_t *kind,
    FILE **file_out,
    size_t *size_out
)
{
    if (file_out == NULL || size_out == NULL) return ESP_ERR_INVALID_ARG;
    *file_out = NULL;
    *size_out = 0U;

    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) return err;

    FILE *file = NULL;
    err = web_scan_recover_file(final_path, temp_path, backup_path);
    if (err == ESP_OK) {
        file = fopen(final_path, "rb");
        if (file == NULL) err = errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    if (err == ESP_OK && fseek(file, 0L, SEEK_END) != 0) err = ESP_FAIL;
    long file_size = 0L;
    if (err == ESP_OK) {
        file_size = ftell(file);
        if (!web_scan_kind_size_valid(kind, file_size)) {
            err = ESP_ERR_INVALID_SIZE;
        }
    }
    if (err == ESP_OK && fseek(file, 0L, SEEK_SET) != 0) err = ESP_FAIL;

    if (err != ESP_OK && file != NULL) {
        (void)fclose(file);
        file = NULL;
    }

    storage_manager_operation_end();

    if (err == ESP_OK) {
        *file_out = file;
        *size_out = (size_t)file_size;
    }
    return err;
}


typedef struct
{
    FILE *file;
    unsigned char *buffer;
    size_t capacity;
    size_t *read_out;

} web_scan_read_chunk_context_t;


static esp_err_t web_scan_read_chunk(void *context)
{
    web_scan_read_chunk_context_t *chunk = context;
    if (
        chunk == NULL || chunk->file == NULL || chunk->buffer == NULL ||
        chunk->capacity == 0U || chunk->read_out == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }
    *chunk->read_out = 0U;

    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) return err;

    const size_t count = fread(
        chunk->buffer,
        1U,
        chunk->capacity,
        chunk->file
    );
    if (ferror(chunk->file) != 0) err = ESP_FAIL;
    *chunk->read_out = count;

    storage_manager_operation_end();
    return err;
}


static esp_err_t web_scan_load(httpd_req_t *request, const char *object_id, const char *session_id, const web_scan_kind_t *kind)
{
    if (
        web_scan_kind_uses_bulk_admission(kind) &&
        storage_bulk_admission_is_protected()
    ) {
        return web_scan_send_recording_busy(request, "scan_load");
    }

    esp_err_t err = web_scan_verify_object(object_id);
    if (err == ESP_OK) err = web_scan_verify_session(object_id, session_id);
    if (err != ESP_OK) return web_scan_send_json(request, "scan_load", object_id, session_id, err, 0U);

    char final_path[WEB_SCAN_PATH_SIZE], temp_path[WEB_SCAN_PATH_SIZE], backup_path[WEB_SCAN_PATH_SIZE];
    err = web_scan_paths(object_id, session_id, kind, final_path, temp_path, backup_path);
    if (err != ESP_OK) return web_scan_send_json(request, "scan_load", object_id, session_id, err, 0U);

    const int64_t started_us = esp_timer_get_time();
    FILE *file = NULL;
    size_t size = 0U;
    err = web_scan_open_for_stream(
        final_path,
        temp_path,
        backup_path,
        kind,
        &file,
        &size
    );
    if (err != ESP_OK) {
        return web_scan_send_json(request, "scan_load", object_id, session_id, err, 0U);
    }

    unsigned char *buffer = heap_caps_malloc(
        WEB_SCAN_IO_BUFFER_SIZE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (buffer == NULL) {
        (void)web_scan_close_reader(&file);
        return web_scan_send_json(request, "scan_load", object_id, session_id, ESP_ERR_NO_MEM, 0U);
    }

    const web_scan_priority_guard_t priority_guard = web_scan_priority_enter();

    char size_header[24];
    (void)snprintf(size_header, sizeof(size_header), "%u", (unsigned)size);
    httpd_resp_set_type(request, kind == &WEB_SCAN_KIND_REGISTRATION
        ? "application/json; charset=utf-8"
        : "application/octet-stream");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Geo-Scan-Bytes", size_header);

    size_t sent_total = 0U;
    while (sent_total < size) {
        const size_t remaining = size - sent_total;
        const size_t request_size = remaining < WEB_SCAN_IO_BUFFER_SIZE
            ? remaining
            : WEB_SCAN_IO_BUFFER_SIZE;
        size_t read_count = 0U;

        web_scan_read_chunk_context_t chunk = {
            .file = file,
            .buffer = buffer,
            .capacity = request_size,
            .read_out = &read_count,
        };
        err = web_scan_kind_uses_bulk_admission(kind)
            ? storage_bulk_admission_run_chunk(
                web_scan_read_chunk,
                &chunk
            )
            : web_scan_read_chunk(&chunk);
        if (err != ESP_OK || read_count == 0U) {
            if (err == ESP_OK) err = ESP_FAIL;
            break;
        }

        /* Socket backpressure is deliberately outside storage_manager. */
        err = httpd_resp_send_chunk(
            request,
            (const char *)buffer,
            (ssize_t)read_count
        );
        if (err != ESP_OK) break;
        sent_total += read_count;
    }

    const esp_err_t close_err = web_scan_close_reader(&file);
    if (err == ESP_OK) err = close_err;

    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(request, NULL, 0);
    }

    web_scan_priority_leave(priority_guard);
    free(buffer);

    const uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - started_us) / 1000);
    if (err == ESP_OK) {
        ESP_LOGI(
            TAG,
            "Loaded 3D scan: object=%s bytes=%u elapsed=%u ms chunked_storage_windows=1",
            object_id,
            (unsigned)sent_total,
            (unsigned)elapsed_ms
        );
    }
    else {
        ESP_LOGE(
            TAG,
            "3D scan stream failed: object=%s sent=%u/%u err=%s",
            object_id,
            (unsigned)sent_total,
            (unsigned)size,
            esp_err_to_name(err)
        );
    }
    return err;
}


static esp_err_t web_scan_delete(httpd_req_t *request, const char *object_id, const char *session_id, const web_scan_kind_t *kind)
{
    if (
        web_scan_kind_uses_bulk_admission(kind) &&
        storage_bulk_admission_is_protected()
    ) {
        return web_scan_send_recording_busy(request, "scan_delete");
    }

    esp_err_t err = web_scan_verify_object(object_id);
    if (err == ESP_OK) err = web_scan_verify_session(object_id, session_id);
    if (err != ESP_OK) return web_scan_send_json(request, "scan_delete", object_id, session_id, err, 0U);
    if (kind == &WEB_SCAN_KIND_REGISTRATION && web_scan_registration_session_is_recording(object_id, session_id)) {
        return web_scan_send_registration_conflict(request);
    }

    char final_path[WEB_SCAN_PATH_SIZE], temp_path[WEB_SCAN_PATH_SIZE], backup_path[WEB_SCAN_PATH_SIZE];
    err = web_scan_paths(object_id, session_id, kind, final_path, temp_path, backup_path);
    if (err != ESP_OK) return web_scan_send_json(request, "scan_delete", object_id, session_id, err, 0U);

    err = storage_manager_operation_begin();
    if (err != ESP_OK) return web_scan_send_json(request, "scan_delete", object_id, session_id, err, 0U);
    if (remove(final_path) != 0 && errno != ENOENT) err = ESP_FAIL;
    if (remove(temp_path) != 0 && errno != ENOENT && err == ESP_OK) err = ESP_FAIL;
    if (remove(backup_path) != 0 && errno != ENOENT && err == ESP_OK) err = ESP_FAIL;
    if (err == ESP_OK && kind == &WEB_SCAN_KIND_SCAN) {
        /* Both derivatives are tied to the scan CRC: never leave either
         * occupancy constraints or a display preview behind as stale data. */
        char occ_final[WEB_SCAN_PATH_SIZE], occ_temp[WEB_SCAN_PATH_SIZE], occ_backup[WEB_SCAN_PATH_SIZE];
        if (web_scan_paths(object_id, session_id, &WEB_SCAN_KIND_OCCUPANCY, occ_final, occ_temp, occ_backup) == ESP_OK) {
            if (remove(occ_final) != 0 && errno != ENOENT) err = ESP_FAIL;
            if (remove(occ_temp) != 0 && errno != ENOENT && err == ESP_OK) err = ESP_FAIL;
            if (remove(occ_backup) != 0 && errno != ENOENT && err == ESP_OK) err = ESP_FAIL;
        }
        char preview_final[WEB_SCAN_PATH_SIZE], preview_temp[WEB_SCAN_PATH_SIZE], preview_backup[WEB_SCAN_PATH_SIZE];
        if (web_scan_paths(object_id, session_id, &WEB_SCAN_KIND_PREVIEW, preview_final, preview_temp, preview_backup) == ESP_OK) {
            if (remove(preview_final) != 0 && errno != ENOENT && err == ESP_OK) err = ESP_FAIL;
            if (remove(preview_temp) != 0 && errno != ENOENT && err == ESP_OK) err = ESP_FAIL;
            if (remove(preview_backup) != 0 && errno != ENOENT && err == ESP_OK) err = ESP_FAIL;
        }
    }
    storage_manager_operation_end();

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Deleted saved %s: object=%s session=%s", kind->label, object_id, session_id);
    }
    return web_scan_send_json(request, "scan_delete", object_id, session_id, err, 0U);
}


/* MAX-169: does this session have a scan / occupancy artifact, and how big
 * is it -- without streaming megabytes over the ESP link. Absent is
 * {"ok":false,...,"error":"ESP_ERR_NOT_FOUND"}; present is {"ok":true,"bytes":N}. */
static esp_err_t web_scan_info(httpd_req_t *request, const char *object_id, const char *session_id, const web_scan_kind_t *kind)
{
    esp_err_t err = web_scan_verify_object(object_id);
    if (err == ESP_OK) err = web_scan_verify_session(object_id, session_id);
    if (err != ESP_OK) return web_scan_send_json(request, "scan_info", object_id, session_id, err, 0U);

    char final_path[WEB_SCAN_PATH_SIZE], temp_path[WEB_SCAN_PATH_SIZE], backup_path[WEB_SCAN_PATH_SIZE];
    err = web_scan_paths(object_id, session_id, kind, final_path, temp_path, backup_path);
    if (err != ESP_OK) return web_scan_send_json(request, "scan_info", object_id, session_id, err, 0U);

    FILE *file = NULL;
    size_t size = 0U;
    err = web_scan_open_for_stream(final_path, temp_path, backup_path, kind, &file, &size);
    if (err == ESP_OK) {
        err = web_scan_close_reader(&file);
    }
    return web_scan_send_json(request, "scan_info", object_id, session_id, err, err == ESP_OK ? size : 0U);
}


esp_err_t web_scan_store_try_handle(httpd_req_t *request, bool *handled)
{
    if (request == NULL || handled == NULL) return ESP_ERR_INVALID_ARG;
    *handled = false;

    char action[16];
    esp_err_t err = web_scan_query_value(request, "scan", action, sizeof(action));
    if (err == ESP_ERR_NOT_FOUND) return ESP_OK;
    *handled = true;
    if (err != ESP_OK) return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid scan query");

    char object_id[OBJECT_STORE_ID_SIZE];
    err = web_scan_query_value(request, "object_id", object_id, sizeof(object_id));
    if (err != ESP_OK || !object_format_id_is_valid(object_id)) {
        return web_scan_send_json(request, "scan", "INVALID", "INVALID", ESP_ERR_INVALID_ARG, 0U);
    }

    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE];
    err = web_scan_query_value(request, "session_id", session_id, sizeof(session_id));
    if (err != ESP_OK) {
        return web_scan_send_json(request, "scan", object_id, "INVALID", ESP_ERR_INVALID_ARG, 0U);
    }

    /* Optional `kind`: absent or "scan" = the full scan artifact. */
    const web_scan_kind_t *kind = &WEB_SCAN_KIND_SCAN;
    char kind_value[16];
    err = web_scan_query_value(request, "kind", kind_value, sizeof(kind_value));
    if (err == ESP_OK) {
        if (strcmp(kind_value, "occupancy") == 0) {
            kind = &WEB_SCAN_KIND_OCCUPANCY;
        } else if (strcmp(kind_value, "preview") == 0) {
            kind = &WEB_SCAN_KIND_PREVIEW;
        } else if (strcmp(kind_value, "registration") == 0) {
            kind = &WEB_SCAN_KIND_REGISTRATION;
        } else if (strcmp(kind_value, "scan") != 0) {
            return web_scan_send_json(request, "scan", object_id, session_id, ESP_ERR_INVALID_ARG, 0U);
        }
    } else if (err != ESP_ERR_NOT_FOUND) {
        return web_scan_send_json(request, "scan", object_id, session_id, ESP_ERR_INVALID_ARG, 0U);
    }

    if (strcmp(action, "save") == 0) return web_scan_save(request, object_id, session_id, kind);
    if (strcmp(action, "load") == 0) return web_scan_load(request, object_id, session_id, kind);
    if (strcmp(action, "delete") == 0) return web_scan_delete(request, object_id, session_id, kind);
    if (strcmp(action, "info") == 0) return web_scan_info(request, object_id, session_id, kind);
    return web_scan_send_json(request, "scan", object_id, session_id, ESP_ERR_NOT_SUPPORTED, 0U);
}
