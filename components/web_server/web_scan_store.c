#include "web_server_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* MSVC's <sys/stat.h> has S_IFREG but not the POSIX test macro. */
#if !defined(S_ISREG) && defined(S_IFMT) && defined(S_IFREG)
#define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
#endif
#include "app_task_config.h"
#include "cJSON.h"
#include "web_scan_sha256.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "object_format.h"
#include "object_store.h"
#include "seismic_recorder.h"
#include "seismic_recorder_tag_layout.h"
#include "scan_artifact_path.h"
#include "storage_bulk_admission.h"
#include "storage_manager.h"

/* The whole query string, scan_sha (64 hex) included: save/occupancy/object/session/scan_id/scan_sha is about 190 bytes for the longest ids. */
#define WEB_SCAN_QUERY_SIZE       256U
#define WEB_SCAN_PATH_SIZE        192U
#define WEB_SCAN_IO_BUFFER_SIZE   4096U
#define WEB_SCAN_HEADER_PEEK_MAX_BYTES 4096U
#define WEB_SCAN_CRC32_ID_SIZE    20U
#define WEB_SCAN_MAX_BYTES        (8U * 1024U * 1024U)
#define WEB_REGISTRATION_MAX_BYTES 16384U
#define WEB_SCAN_PREVIEW_MAX_BYTES 65536U
#define WEB_SCAN_RECV_TIMEOUTS    5U
#define WEB_SCAN_DRAIN_BUFFER_SIZE 256U

static const char *TAG = "WEB_SCAN_STORE";
/* Object-selection hook: call with the object being left. */
esp_err_t web_scan_store_clear_staging(const char *object_id);
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

/*
 * The tag-derived sensor layout the phone computed (tag-positions-v1.json): a JSON file that names the scan it belongs to by `scan_frame_id`,
 * like the registration, so it is committed, bound, replaced and removed the same way. Unlike the registration it is not tied to a
 * localization geometry: it is an alternative layout stored beside the Channel Sounding one, never over it.
 */
static const web_scan_kind_t WEB_SCAN_KIND_TAGS = {
    .file_name = SEISMIC_RECORDER_TAG_POSITIONS_FILE,
    .max_bytes = SEISMIC_RECORDER_TAG_POSITIONS_MAX_BYTES,
    .label = "AprilTag sensor positions",
};

/* The JSON kinds that name their scan: committed only when bound to the scan that is current. */
static bool web_scan_kind_is_bound_json(const web_scan_kind_t *kind)
{
    return kind == &WEB_SCAN_KIND_REGISTRATION || kind == &WEB_SCAN_KIND_TAGS;
}

typedef struct
{
    bool valid;
    char path[WEB_SCAN_PATH_SIZE];
    size_t size;
    uint32_t crc32;

} web_scan_identity_cache_t;

static web_scan_identity_cache_t s_scan_identity_cache;

/* SHA-256 of the scan the identity cache names (issue #34): filled lazily, dropped together with the identity whenever the scan changes. */
typedef struct
{
    bool valid;
    char path[WEB_SCAN_PATH_SIZE];
    uint32_t crc32;
    unsigned char sha256[WEB_SCAN_SHA256_BYTES];
} web_scan_sha_cache_t;

static web_scan_sha_cache_t s_scan_sha_cache;

static esp_err_t web_scan_get_scan_identity(
    const char *object_id,
    const char *session_id,
    bool *present_out,
    size_t *size_out,
    uint32_t *crc_out
);
static esp_err_t web_scan_current_scan_sha256(
    const char *object_id,
    const char *session_id,
    uint32_t expected_crc32,
    unsigned char sha_out[WEB_SCAN_SHA256_BYTES]
);
static esp_err_t web_scan_validate_path_locked(
    const char *path,
    const web_scan_kind_t *kind,
    bool *valid_out,
    uint32_t *source_crc_out,
    bool *registration_bound_out
);
static esp_err_t web_scan_validate_uploaded_temp(
    const char *temp_path,
    const web_scan_kind_t *kind,
    uint32_t *source_crc_out,
    bool *registration_bound_out,
    bool *valid_out
);
#if defined(WEB_SCAN_STORE_TESTING)
void web_scan_store_test_before_derivative_commit(void);
void web_scan_store_test_before_identity_cache_store(void);
#endif

static uint32_t web_scan_crc32_update(uint32_t crc, const unsigned char *bytes, size_t length)
{
    if (bytes == NULL) return crc;
    for (size_t i = 0U; i < length; ++i) {
        crc ^= (uint32_t)bytes[i];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return crc;
}

static void web_scan_format_crc32_id(uint32_t crc, char output[WEB_SCAN_CRC32_ID_SIZE])
{
    static const char hex[] = "0123456789abcdef";
    static const char prefix[] = "scan-crc32:";
    memcpy(output, prefix, sizeof(prefix) - 1U);
    for (unsigned digit = 0U; digit < 8U; ++digit) {
        const unsigned shift = (7U - digit) * 4U;
        output[(sizeof(prefix) - 1U) + digit] = hex[(crc >> shift) & 0x0fU];
    }
    output[WEB_SCAN_CRC32_ID_SIZE - 1U] = '\0';
}

static bool web_scan_parse_crc32_id(const char *value, uint32_t *crc_out)
{
    static const char prefix[] = "scan-crc32:";
    if (value == NULL || crc_out == NULL || strlen(value) != WEB_SCAN_CRC32_ID_SIZE - 1U) return false;
    if (memcmp(value, prefix, sizeof(prefix) - 1U) != 0) return false;

    uint32_t crc = 0U;
    for (size_t i = sizeof(prefix) - 1U; i < WEB_SCAN_CRC32_ID_SIZE - 1U; ++i) {
        const char digit = value[i];
        unsigned nibble = 0U;
        if (digit >= '0' && digit <= '9') nibble = (unsigned)(digit - '0');
        else if (digit >= 'a' && digit <= 'f') nibble = (unsigned)(digit - 'a') + 10U;
        else return false;
        crc = (crc << 4U) | (uint32_t)nibble;
    }
    *crc_out = crc;
    return true;
}

static void web_scan_identity_cache_clear_locked(const char *path)
{
    if (path == NULL || (s_scan_identity_cache.valid && strcmp(s_scan_identity_cache.path, path) == 0)) {
        s_scan_identity_cache.valid = false;
        s_scan_sha_cache.valid = false;
    }
}

static void web_scan_identity_cache_set_locked(const char *path, size_t size, uint32_t crc32)
{
    s_scan_sha_cache.valid = false; /* a new identity is a new scan: its digest is unknown until computed */
    if (path == NULL) {
        s_scan_identity_cache.valid = false;
        return;
    }
    const size_t path_length = strlen(path);
    if (path_length >= sizeof(s_scan_identity_cache.path)) {
        s_scan_identity_cache.valid = false;
        return;
    }
    memcpy(s_scan_identity_cache.path, path, path_length + 1U);
    s_scan_identity_cache.size = size;
    s_scan_identity_cache.crc32 = crc32;
    s_scan_identity_cache.valid = true;
}

static bool web_scan_kind_size_valid(const web_scan_kind_t *kind, long size)
{
    return kind != NULL && size > 0L && (unsigned long)size <= kind->max_bytes;
}

static bool web_scan_session_is_staging(const char *session_id)
{
    return session_id != NULL && strcmp(session_id, "staging") == 0;
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


static esp_err_t web_scan_send_json_extra(
    httpd_req_t *request,
    const char *action,
    const char *object_id,
    const char *session_id,
    esp_err_t result,
    size_t bytes,
    const char *extra_key,
    const char *extra_value,
    bool extra_is_null
)
{
    char body[320];
    int written = 0;
    if (result == ESP_OK) {
        if (extra_key == NULL) {
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
        else if (extra_is_null) {
            written = snprintf(
                body,
                sizeof(body),
                "{\"ok\":true,\"action\":\"%s\",\"object_id\":\"%s\",\"session_id\":\"%s\",\"bytes\":%u,\"%s\":null}",
                action,
                object_id,
                session_id,
                (unsigned)bytes,
                extra_key
            );
        }
        else {
            written = snprintf(
                body,
                sizeof(body),
                "{\"ok\":true,\"action\":\"%s\",\"object_id\":\"%s\",\"session_id\":\"%s\",\"bytes\":%u,\"%s\":\"%s\"}",
                action,
                object_id,
                session_id,
                (unsigned)bytes,
                extra_key,
                extra_value
            );
        }
    }
    else {
        if (extra_key == NULL) {
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
        else if (extra_is_null) {
            written = snprintf(
                body,
                sizeof(body),
                "{\"ok\":false,\"action\":\"%s\",\"object_id\":\"%s\",\"session_id\":\"%s\",\"error\":\"%s\",\"%s\":null}",
                action,
                object_id,
                session_id,
                esp_err_to_name(result),
                extra_key
            );
        }
        else {
            written = snprintf(
                body,
                sizeof(body),
                "{\"ok\":false,\"action\":\"%s\",\"object_id\":\"%s\",\"session_id\":\"%s\",\"error\":\"%s\",\"%s\":\"%s\"}",
                action,
                object_id,
                session_id,
                esp_err_to_name(result),
                extra_key,
                extra_value
            );
        }
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


static esp_err_t web_scan_send_json(
    httpd_req_t *request,
    const char *action,
    const char *object_id,
    const char *session_id,
    esp_err_t result,
    size_t bytes
)
{
    return web_scan_send_json_extra(
        request,
        action,
        object_id,
        session_id,
        result,
        bytes,
        NULL,
        NULL,
        false
    );
}


static esp_err_t web_scan_send_generation_mismatch(httpd_req_t *request, const char *current_id)
{
    char body[128];
    const int written = current_id == NULL
        ? snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"scan_generation_mismatch\",\"current_id\":null}")
        : snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"scan_generation_mismatch\",\"current_id\":\"%s\"}", current_id);
    if (written <= 0 || (size_t)written >= sizeof(body)) return ESP_ERR_INVALID_SIZE;

    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_status(request, "409 Conflict");
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
    if (!object_format_id_is_valid(object_id) || session_id == NULL || kind == NULL) return ESP_ERR_INVALID_ARG;
    /* MAX-167: a scan belongs to one recording/session, under that session's
     * own directory -- never directly under the object. */
    const int final_written = web_scan_session_is_staging(session_id)
        ? snprintf(
            final_path,
            WEB_SCAN_PATH_SIZE,
            "%s/%s/staging/%s",
            STORAGE_MANAGER_OBJECTS_PATH,
            object_id,
            kind->file_name
        )
        : snprintf(
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


/*
 * The paths to READ a session's artifact from. Writing, replacing and removing always address the session's own file; a
 * read also sees the shared scan of the session's sensor placement (several recordings refer to one scan, see
 * scan_artifact_path.h), unless the session has its own copy. The lookup opens files, so it runs in a storage operation.
 */
static esp_err_t web_scan_paths_for_read(
    const char *object_id,
    const char *session_id,
    const web_scan_kind_t *kind,
    char final_path[WEB_SCAN_PATH_SIZE],
    char temp_path[WEB_SCAN_PATH_SIZE],
    char backup_path[WEB_SCAN_PATH_SIZE]
)
{
    esp_err_t err = web_scan_paths(object_id, session_id, kind, final_path, temp_path, backup_path);
    if (err != ESP_OK || web_scan_session_is_staging(session_id)) return err;
    err = storage_manager_operation_begin();
    if (err != ESP_OK) return ESP_OK; /* without the card there is nothing more to find: keep the session's own path */
    char resolved[WEB_SCAN_PATH_SIZE];
    const int result = scan_artifact_resolve(
        STORAGE_MANAGER_OBJECTS_PATH, object_id, session_id, kind->file_name, resolved, sizeof(resolved));
    storage_manager_operation_end();
    if (result != 0) return ESP_ERR_INVALID_SIZE;
    const int final_written = snprintf(final_path, WEB_SCAN_PATH_SIZE, "%s", resolved);
    const int temp_written = snprintf(temp_path, WEB_SCAN_PATH_SIZE, "%s.tmp", final_path);
    const int backup_written = snprintf(backup_path, WEB_SCAN_PATH_SIZE, "%s.bak", final_path);
    if (final_written <= 0 || temp_written <= 0 || backup_written <= 0 ||
        final_written >= (int)WEB_SCAN_PATH_SIZE || temp_written >= (int)WEB_SCAN_PATH_SIZE ||
        backup_written >= (int)WEB_SCAN_PATH_SIZE) {
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
    if (object_id == NULL || session_id == NULL) return ESP_ERR_INVALID_ARG;
    if (web_scan_session_is_staging(session_id)) return ESP_OK;

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

static bool web_scan_requires_bulk_admission(
    const web_scan_kind_t *kind,
    const char *session_id
)
{
    return web_scan_kind_uses_bulk_admission(kind) || web_scan_session_is_staging(session_id);
}

static esp_err_t web_scan_ensure_staging_directory(const char *object_id, const char *session_id)
{
    if (!web_scan_session_is_staging(session_id)) return ESP_OK;

    char path[WEB_SCAN_PATH_SIZE];
    const int written = snprintf(
        path,
        sizeof(path),
        "%s/%s/staging",
        STORAGE_MANAGER_OBJECTS_PATH,
        object_id
    );
    if (written <= 0 || (size_t)written >= sizeof(path)) return ESP_ERR_INVALID_SIZE;

    return storage_manager_create_directory(path);
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
static esp_err_t web_scan_recover_file_locked(
    const char *final_path,
    const char *temp_path,
    const char *backup_path,
    const web_scan_kind_t *kind
)
{
    const bool final_exists = web_scan_file_exists(final_path);
    const bool temp_exists = web_scan_file_exists(temp_path);
    const bool backup_exists = web_scan_file_exists(backup_path);
    if (!temp_exists && !backup_exists) {
        return final_exists ? ESP_OK : ESP_ERR_NOT_FOUND;
    }

    bool final_valid = false;
    bool backup_valid = false;
    esp_err_t err = ESP_OK;
    if (final_exists) {
        err = web_scan_validate_path_locked(final_path, kind, &final_valid, NULL, NULL);
        if (err != ESP_OK) return err;
    }
    if (backup_exists) {
        err = web_scan_validate_path_locked(backup_path, kind, &backup_valid, NULL, NULL);
        if (err != ESP_OK) return err;
    }

    if (final_valid) {
        if (remove(temp_path) != 0 && errno != ENOENT) return ESP_FAIL;
        if (remove(backup_path) != 0 && errno != ENOENT) return ESP_FAIL;
        return ESP_OK;
    }

    if (backup_valid) {
        if (final_exists && remove(final_path) != 0) return ESP_FAIL;
        if (remove(temp_path) != 0 && errno != ENOENT) return ESP_FAIL;
        if (rename(backup_path, final_path) != 0) return ESP_FAIL;
        if (kind == &WEB_SCAN_KIND_SCAN) web_scan_identity_cache_clear_locked(final_path);
        ESP_LOGW(TAG, "Recovered %s from backup", kind->label);
        return ESP_OK;
    }

    if (!final_exists && !backup_exists) {
        if (remove(temp_path) != 0 && errno != ENOENT) return ESP_FAIL;
        return ESP_ERR_NOT_FOUND;
    }

    if (kind == &WEB_SCAN_KIND_SCAN) web_scan_identity_cache_clear_locked(final_path);
    return ESP_ERR_INVALID_RESPONSE;
}


typedef struct
{
    const char *final_path;
    const char *temp_path;
    const char *backup_path;
    const web_scan_kind_t *kind;

} web_scan_recovery_context_t;


static esp_err_t web_scan_recovery_chunk(void *context)
{
    web_scan_recovery_context_t *recovery = context;
    if (recovery == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) return err;
    err = web_scan_recover_file_locked(
        recovery->final_path,
        recovery->temp_path,
        recovery->backup_path,
        recovery->kind
    );
    storage_manager_operation_end();
    return err;
}


static esp_err_t web_scan_recover_file(
    const char *final_path,
    const char *temp_path,
    const char *backup_path,
    const web_scan_kind_t *kind
)
{
    if (final_path == NULL || temp_path == NULL || backup_path == NULL || kind == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /*
     * Board 2026-11-17: these three stat() calls ran with no storage operation held, from the httpd task, while the recorder or
     * the storage task was on the SD's SPI bus. Two tasks in one SPI device is an assert in the driver
     * (spi_hal_setup_trans: spi_ll_get_running_cmd == 0) and the board rebooted on every scan-info request. The guard is
     * recursive, so a caller that already holds it is unaffected.
     */
    esp_err_t guard = storage_manager_operation_begin();
    if (guard != ESP_OK) return guard;
    const bool recovery_pending = web_scan_file_exists(temp_path) || web_scan_file_exists(backup_path);
    const bool final_present = recovery_pending ? false : web_scan_file_exists(final_path);
    storage_manager_operation_end();
    if (!recovery_pending) {
        return final_present ? ESP_OK : ESP_ERR_NOT_FOUND;
    }
    const web_scan_recovery_context_t context = {
        .final_path = final_path,
        .temp_path = temp_path,
        .backup_path = backup_path,
        .kind = kind,
    };
    /* Admission is acquired before storage. Recovery validates at most 8 MiB
     * in fixed chunks, then makes its decision under the same storage lock as
     * scan commits, so no writer can replace a candidate during validation. */
    return storage_bulk_admission_run_chunk(web_scan_recovery_chunk, (void *)&context);
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


/* The caller holds storage_manager_operation_begin() while committing paths. */
static esp_err_t web_scan_commit_temp_locked(
    const char *final_path,
    const char *temp_path,
    const char *backup_path,
    bool cache_scan,
    size_t scan_size,
    uint32_t scan_crc32
)
{
    (void)remove(backup_path);
    const bool had_final = web_scan_file_exists(final_path);
    esp_err_t err = ESP_OK;
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

    if (cache_scan) {
        if (err == ESP_OK) web_scan_identity_cache_set_locked(final_path, scan_size, scan_crc32);
        else web_scan_identity_cache_clear_locked(final_path);
    }
    return err;
}


static void web_scan_remove_temp_best_effort(const char *temp_path)
{
    if (storage_manager_operation_begin() != ESP_OK) return;
    (void)remove(temp_path);
    storage_manager_operation_end();
}


/* The caller holds storage_manager_operation_begin() while removing paths. */
static esp_err_t web_scan_remove_paths_locked(
    const char *final_path,
    const char *temp_path,
    const char *backup_path
)
{
    const char *const paths[] = {final_path, temp_path, backup_path};
    for (size_t i = 0U; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        if (remove(paths[i]) != 0 && errno != ENOENT) return ESP_FAIL;
    }
    return ESP_OK;
}


static esp_err_t web_scan_remove_kind_locked(
    const char *object_id,
    const char *session_id,
    const web_scan_kind_t *kind
)
{
    char final_path[WEB_SCAN_PATH_SIZE];
    char temp_path[WEB_SCAN_PATH_SIZE];
    char backup_path[WEB_SCAN_PATH_SIZE];
    const esp_err_t err = web_scan_paths(
        object_id,
        session_id,
        kind,
        final_path,
        temp_path,
        backup_path
    );
    if (err != ESP_OK) return err;
    return web_scan_remove_paths_locked(final_path, temp_path, backup_path);
}


/*
 * Scan replacement and derivative removal use one storage lock. Derivative
 * compare-and-commit takes the same lock, so no derivative can land between
 * invalidation and the scan rename. Bulk-admission callbacks acquire this
 * storage lock only after the admission mutex; commit paths never acquire
 * admission while holding storage.
 */
static bool web_scan_parse_registration_json(char *json, size_t length, uint32_t *source_crc_out, bool *bound_out);

/*
 * Issue #31: a registration describes the camera frame of ONE scan. When the scan is replaced by a different one (other CRC), the registration
 * bound to the old scan must not stay active: its project-frame geophone positions would pass every geometry-generation check and be taken
 * for the new scan's frame. It is retired BEFORE the scan rename (a power cut leaves no stale registration): the file is kept as
 * registration-v1.json.stale for the record, nothing reads it, and its .tmp/.bak are removed so recovery cannot bring it back. A registration
 * already bound to exactly the scan being committed (a byte-identical re-upload) stays untouched.
 */
static esp_err_t web_scan_retire_stale_registration_locked(const char *object_id, const char *session_id, uint32_t new_scan_crc32)
{
    char final_path[WEB_SCAN_PATH_SIZE], temp_path[WEB_SCAN_PATH_SIZE], backup_path[WEB_SCAN_PATH_SIZE];
    esp_err_t err = web_scan_paths(object_id, session_id, &WEB_SCAN_KIND_REGISTRATION, final_path, temp_path, backup_path);
    if (err != ESP_OK) return err;

    const bool has_final = web_scan_file_exists(final_path);
    if (has_final) {
        char *json = malloc(WEB_REGISTRATION_MAX_BYTES + 1U);
        if (json == NULL) return ESP_ERR_NO_MEM;
        bool matches = false;
        FILE *file = fopen(final_path, "rb");
        if (file != NULL) {
            const size_t length = fread(json, 1U, WEB_REGISTRATION_MAX_BYTES, file);
            (void)fclose(file);
            uint32_t bound_crc32 = 0U;
            bool bound = false;
            if (length > 0U && web_scan_parse_registration_json(json, length, &bound_crc32, &bound)) {
                matches = bound && bound_crc32 == new_scan_crc32;
            }
        }
        free(json);
        if (matches) return ESP_OK;

        char stale_path[WEB_SCAN_PATH_SIZE];
        if (snprintf(stale_path, sizeof(stale_path), "%s.stale", final_path) >= (int)sizeof(stale_path)) return ESP_ERR_INVALID_SIZE;
        if (remove(stale_path) != 0 && errno != ENOENT) return ESP_FAIL;
        if (rename(final_path, stale_path) != 0) return ESP_FAIL;
    }
    return web_scan_remove_paths_locked(final_path, temp_path, backup_path);
}


static esp_err_t web_scan_commit_scan_temp(
    const char *object_id,
    const char *session_id,
    const char *final_path,
    const char *temp_path,
    const char *backup_path,
    size_t scan_size,
    uint32_t scan_crc32
)
{
    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) return err;

    /* Delete before scan rename: a power cut leaves no stale derivative. */
    err = web_scan_remove_kind_locked(object_id, session_id, &WEB_SCAN_KIND_OCCUPANCY);
    if (err == ESP_OK) {
        err = web_scan_remove_kind_locked(object_id, session_id, &WEB_SCAN_KIND_PREVIEW);
    }
    if (err == ESP_OK) {
        err = web_scan_remove_kind_locked(object_id, session_id, &WEB_SCAN_KIND_TAGS);
    }
    if (err == ESP_OK) {
        err = web_scan_retire_stale_registration_locked(object_id, session_id, scan_crc32);
    }
    if (err == ESP_OK) {
        err = web_scan_commit_temp_locked(
            final_path,
            temp_path,
            backup_path,
            true,
            scan_size,
            scan_crc32
        );
    }
    if (err != ESP_OK) {
        web_scan_identity_cache_clear_locked(final_path);
    }
    storage_manager_operation_end();
    return err;
}


static esp_err_t web_scan_send_registration_unbound(httpd_req_t *request)
{
    static const char body[] = "{\"ok\":false,\"error\":\"registration_unbound\"}";
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_status(request, "409 Conflict");
    return httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
}


static esp_err_t web_scan_commit_derivative_temp(
    const char *object_id,
    const char *session_id,
    const char *final_path,
    const char *temp_path,
    const char *backup_path,
    uint32_t source_crc32,
    const char *requested_scan_id,
    bool query_present,
    bool query_valid,
    const unsigned char *requested_sha256,
    bool sha_query_present,
    bool sha_query_valid,
    bool *generation_mismatch_out,
    char current_id[WEB_SCAN_CRC32_ID_SIZE]
)
{
    if (generation_mismatch_out == NULL || current_id == NULL) return ESP_ERR_INVALID_ARG;
    *generation_mismatch_out = false;
    current_id[0] = '\0';

    bool current_present = false;
    size_t current_size = 0U;
    uint32_t current_crc32 = 0U;
    esp_err_t err = web_scan_get_scan_identity(
        object_id,
        session_id,
        &current_present,
        &current_size,
        &current_crc32
    );
    (void)current_size;
    if (err == ESP_OK && current_present) web_scan_format_crc32_id(current_crc32, current_id);
    else if (err != ESP_ERR_NOT_FOUND && err != ESP_OK) return err;

    char content_id[WEB_SCAN_CRC32_ID_SIZE];
    web_scan_format_crc32_id(source_crc32, content_id);
    if (
        !current_present || current_id[0] == '\0' || strcmp(current_id, content_id) != 0 ||
        (query_present && (!query_valid || strcmp(requested_scan_id, content_id) != 0))
    ) {
        *generation_mismatch_out = true;
        web_scan_remove_temp_best_effort(temp_path);
        return ESP_OK;
    }

    /*
     * Issue #34: a client that names the SHA-256 of the scan its sidecar was built for is bound by it, not only by the 32-bit CRC. A different scan
     * with the same CRC32 is refused here, and again under the lock below. A client that sends none is bound by CRC32 as before.
     */
    if (sha_query_present) {
        unsigned char current_sha[WEB_SCAN_SHA256_BYTES];
        bool sha_matches = false;
        if (sha_query_valid && requested_sha256 != NULL) {
            err = web_scan_current_scan_sha256(object_id, session_id, current_crc32, current_sha);
            if (err == ESP_OK) sha_matches = memcmp(current_sha, requested_sha256, WEB_SCAN_SHA256_BYTES) == 0;
            else if (err != ESP_ERR_INVALID_STATE && err != ESP_ERR_NOT_FOUND) return err;
        }
        if (!sha_matches) {
            *generation_mismatch_out = true;
            web_scan_remove_temp_best_effort(temp_path);
            return ESP_OK;
        }
    }

    /* The storage operation lock is the generation lock. Scan replacement
     * holds it across derivative invalidation and scan rename; this final read
     * and derivative rename therefore cannot straddle a scan commit. */
    err = storage_manager_operation_begin();
    if (err != ESP_OK) return err;
#if defined(WEB_SCAN_STORE_TESTING)
    web_scan_store_test_before_derivative_commit();
#endif
    char scan_path[WEB_SCAN_PATH_SIZE];
    char scan_temp_path[WEB_SCAN_PATH_SIZE];
    char scan_backup_path[WEB_SCAN_PATH_SIZE];
    err = web_scan_paths_for_read(
        object_id,
        session_id,
        &WEB_SCAN_KIND_SCAN,
        scan_path,
        scan_temp_path,
        scan_backup_path
    );
    current_present = false;
    current_crc32 = 0U;
    if (
        err == ESP_OK && s_scan_identity_cache.valid &&
        strcmp(s_scan_identity_cache.path, scan_path) == 0
    ) {
        current_present = true;
        current_crc32 = s_scan_identity_cache.crc32;
        web_scan_format_crc32_id(current_crc32, current_id);
    }
    else {
        current_id[0] = '\0';
    }

    const bool sha_still_current = !sha_query_present || (
        sha_query_valid && requested_sha256 != NULL && s_scan_sha_cache.valid &&
        strcmp(s_scan_sha_cache.path, scan_path) == 0 && s_scan_sha_cache.crc32 == current_crc32 &&
        memcmp(s_scan_sha_cache.sha256, requested_sha256, WEB_SCAN_SHA256_BYTES) == 0
    );
    if (
        err == ESP_OK && (!current_present || current_crc32 != source_crc32 || !sha_still_current ||
        (query_present && (!query_valid || strcmp(requested_scan_id, content_id) != 0)))
    ) {
        *generation_mismatch_out = true;
        if (remove(temp_path) != 0 && errno != ENOENT) err = ESP_FAIL;
    }
    else if (err == ESP_OK) {
        err = web_scan_commit_temp_locked(
            final_path,
            temp_path,
            backup_path,
            false,
            0U,
            0U
        );
    }
    storage_manager_operation_end();
    return err;
}


static esp_err_t web_scan_save(httpd_req_t *request, const char *object_id, const char *session_id, const web_scan_kind_t *kind)
{
    if (
        web_scan_requires_bulk_admission(kind, session_id) &&
        storage_bulk_admission_is_protected()
    ) {
        web_scan_drain_request_body(request, 0U, kind->max_bytes);
        return web_scan_send_recording_busy(request, "scan_save");
    }

    if (
        kind != &WEB_SCAN_KIND_OCCUPANCY && kind != &WEB_SCAN_KIND_PREVIEW &&
        !web_scan_kind_size_valid(kind, request->content_len)
    ) {
        return web_scan_send_json(request, "scan_save", object_id, session_id, ESP_ERR_INVALID_SIZE, 0U);
    }

    esp_err_t err = web_scan_verify_object(object_id);
    if (err == ESP_OK) err = web_scan_verify_session(object_id, session_id);
    if (err != ESP_OK) return web_scan_send_json(request, "scan_save", object_id, session_id, err, 0U);
    if (
        web_scan_kind_is_bound_json(kind) && !web_scan_session_is_staging(session_id) &&
        web_scan_registration_session_is_recording(object_id, session_id)
    ) {
        return web_scan_send_registration_conflict(request);
    }

    char requested_scan_id[WEB_SCAN_QUERY_SIZE] = {0};
    const esp_err_t query_err = web_scan_query_value(
        request,
        "scan_id",
        requested_scan_id,
        sizeof(requested_scan_id)
    );
    const bool query_present = query_err != ESP_ERR_NOT_FOUND;
    const bool query_valid = query_err == ESP_OK;
    char requested_sha_hex[WEB_SCAN_QUERY_SIZE] = {0};
    const esp_err_t sha_query_err = web_scan_query_value(request, "scan_sha", requested_sha_hex, sizeof(requested_sha_hex));
    const bool sha_query_present = sha_query_err != ESP_ERR_NOT_FOUND;
    unsigned char requested_sha[WEB_SCAN_SHA256_BYTES] = {0};
    const bool sha_query_valid = sha_query_err == ESP_OK && web_scan_sha256_from_hex(requested_sha_hex, requested_sha);

    if (!web_scan_kind_size_valid(kind, request->content_len)) {
        return web_scan_send_json(request, "scan_save", object_id, session_id, ESP_ERR_INVALID_SIZE, 0U);
    }

    char final_path[WEB_SCAN_PATH_SIZE], temp_path[WEB_SCAN_PATH_SIZE], backup_path[WEB_SCAN_PATH_SIZE];
    err = web_scan_paths(object_id, session_id, kind, final_path, temp_path, backup_path);
    if (err != ESP_OK) return web_scan_send_json(request, "scan_save", object_id, session_id, err, 0U);
    err = web_scan_ensure_staging_directory(object_id, session_id);
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
    uint32_t scan_crc_state = UINT32_C(0xffffffff);
    unsigned timeout_count = 0U;
    bool admission_rejected = false;
    bool generation_rejected = false;
    bool registration_unbound = false;
    char mismatch_current_id[WEB_SCAN_CRC32_ID_SIZE] = {0};

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
            err = web_scan_requires_bulk_admission(kind, session_id)
                ? storage_bulk_admission_run_chunk(
                    web_scan_write_chunk,
                    &chunk
                )
                : web_scan_write_chunk(&chunk);
            if (err == ESP_ERR_INVALID_STATE && !chunk.invoked) {
                admission_rejected = true;
            }
            if (err != ESP_OK) break;
            if (kind == &WEB_SCAN_KIND_SCAN) {
                scan_crc_state = web_scan_crc32_update(scan_crc_state, buffer, (size_t)received);
            }
            received_total += (size_t)received;
        }
    }

    if (file != NULL) {
        const esp_err_t close_err = web_scan_close_writer(&file);
        if (err == ESP_OK) err = close_err;
    }

    if (err == ESP_OK) {
        if (
            kind == &WEB_SCAN_KIND_OCCUPANCY || kind == &WEB_SCAN_KIND_PREVIEW ||
            web_scan_kind_is_bound_json(kind)
        ) {
            bool valid = false;
            bool registration_bound = false;
            uint32_t source_crc32 = 0U;
            err = web_scan_validate_uploaded_temp(
                temp_path,
                kind,
                &source_crc32,
                &registration_bound,
                &valid
            );
            if (err == ESP_OK && !valid) {
                bool current_present = false;
                size_t current_size = 0U;
                uint32_t current_crc32 = 0U;
                const esp_err_t identity_err = web_scan_get_scan_identity(
                    object_id,
                    session_id,
                    &current_present,
                    &current_size,
                    &current_crc32
                );
                (void)current_size;
                if (identity_err == ESP_OK && current_present) {
                    web_scan_format_crc32_id(current_crc32, mismatch_current_id);
                }
                generation_rejected = true;
            }
            else if (err == ESP_OK && web_scan_kind_is_bound_json(kind) && !registration_bound) {
                registration_unbound = true;
            }
            else if (err == ESP_OK) {
                err = web_scan_commit_derivative_temp(
                    object_id,
                    session_id,
                    final_path,
                    temp_path,
                    backup_path,
                    source_crc32,
                    requested_scan_id,
                    query_present,
                    query_valid,
                    requested_sha,
                    sha_query_present,
                    sha_query_valid,
                    &generation_rejected,
                    mismatch_current_id
                );
            }
            if (generation_rejected || registration_unbound) err = ESP_ERR_INVALID_STATE;
        }
        else if (kind == &WEB_SCAN_KIND_SCAN) {
            /* A scan whose final chunk was accepted before the latch is already
             * in-flight work. Finish its existing temp-file commit; do not turn the
             * shutdown admission boundary into a transactional rollback boundary. */
            const uint32_t scan_crc32 = scan_crc_state ^ UINT32_C(0xffffffff);
            err = web_scan_commit_scan_temp(
                object_id,
                session_id,
                final_path,
                temp_path,
                backup_path,
                received_total,
                scan_crc32
            );
        }
    }
    if (err != ESP_OK) {
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
    if (admission_rejected) return web_scan_send_recording_busy(request, "scan_save");
    if (registration_unbound) return web_scan_send_registration_unbound(request);
    if (generation_rejected) {
        return web_scan_send_generation_mismatch(
            request,
            mismatch_current_id[0] == '\0' ? NULL : mismatch_current_id
        );
    }
    if (err == ESP_OK && kind == &WEB_SCAN_KIND_SCAN) {
        char scan_id[WEB_SCAN_CRC32_ID_SIZE];
        web_scan_format_crc32_id(scan_crc_state ^ UINT32_C(0xffffffff), scan_id);
        return web_scan_send_json_extra(
            request,
            "scan_save",
            object_id,
            session_id,
            err,
            received_total,
            "id",
            scan_id,
            false
        );
    }
    return web_scan_send_json(request, "scan_save", object_id, session_id, err, received_total);
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

    esp_err_t err = web_scan_recover_file(final_path, temp_path, backup_path, kind);
    if (err != ESP_OK) return err;

    err = storage_manager_operation_begin();
    if (err != ESP_OK) return err;

    FILE *file = NULL;
    file = fopen(final_path, "rb");
    if (file == NULL) err = errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;

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


static esp_err_t web_scan_identity_cache_lookup(
    const char *path,
    size_t size,
    bool *found_out,
    uint32_t *crc_out
)
{
    if (path == NULL || found_out == NULL || crc_out == NULL) return ESP_ERR_INVALID_ARG;
    *found_out = false;

    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) return err;
    if (
        s_scan_identity_cache.valid &&
        s_scan_identity_cache.size == size &&
        strcmp(s_scan_identity_cache.path, path) == 0
    ) {
        *found_out = true;
        *crc_out = s_scan_identity_cache.crc32;
    }
    storage_manager_operation_end();
    return ESP_OK;
}


static esp_err_t web_scan_identity_cache_store(
    const char *path,
    size_t size,
    uint32_t crc32,
    size_t *size_out,
    uint32_t *crc32_out
)
{
    if (path == NULL || size_out == NULL || crc32_out == NULL) return ESP_ERR_INVALID_ARG;
#if defined(WEB_SCAN_STORE_TESTING)
    web_scan_store_test_before_identity_cache_store();
#endif
    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) return err;
    /* A scan commit may have replaced this path while the opened file was
     * being hashed. Keep the identity published by that commit. */
    if (
        s_scan_identity_cache.valid &&
        strcmp(s_scan_identity_cache.path, path) == 0
    ) {
        *size_out = s_scan_identity_cache.size;
        *crc32_out = s_scan_identity_cache.crc32;
    }
    else if (!web_scan_file_exists(path)) {
        err = ESP_ERR_NOT_FOUND;
    }
    else {
        web_scan_identity_cache_set_locked(path, size, crc32);
        *size_out = size;
        *crc32_out = crc32;
    }
    storage_manager_operation_end();
    return err; /* a scan that vanished after hashing must not be reported present (issue #30) */
}


static esp_err_t web_scan_hash_file(FILE *file, size_t size, uint32_t *crc_out, unsigned char *sha_out)
{
    if (file == NULL || crc_out == NULL) return ESP_ERR_INVALID_ARG;
    unsigned char *buffer = heap_caps_malloc(WEB_SCAN_IO_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) return ESP_ERR_NO_MEM;

    size_t remaining = size;
    uint32_t crc = UINT32_C(0xffffffff);
    web_scan_sha256_t sha;
    if (sha_out != NULL) web_scan_sha256_init(&sha);
    esp_err_t err = ESP_OK;
    while (remaining > 0U) {
        if (web_server_shutdown_requested()) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }

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
        err = storage_bulk_admission_run_chunk(web_scan_read_chunk, &chunk);
        if (err != ESP_OK || read_count == 0U) {
            if (err == ESP_OK) err = ESP_FAIL;
            break;
        }
        crc = web_scan_crc32_update(crc, buffer, read_count);
        if (sha_out != NULL) web_scan_sha256_update(&sha, buffer, read_count);
        remaining -= read_count;
        taskYIELD();
    }

    free(buffer);
    if (sha_out != NULL) {
        if (err == ESP_OK) web_scan_sha256_final(&sha, sha_out);
        else web_scan_sha256_abort(&sha);
    }
    if (err == ESP_OK) *crc_out = crc ^ UINT32_C(0xffffffff);
    return err;
}


static esp_err_t web_scan_crc32_file(FILE *file, size_t size, uint32_t *crc_out)
{
    return web_scan_hash_file(file, size, crc_out, NULL);
}


static esp_err_t web_scan_get_scan_identity(
    const char *object_id,
    const char *session_id,
    bool *present_out,
    size_t *size_out,
    uint32_t *crc_out
)
{
    if (present_out == NULL || size_out == NULL || crc_out == NULL) return ESP_ERR_INVALID_ARG;
    *present_out = false;
    *size_out = 0U;
    *crc_out = 0U;

    char final_path[WEB_SCAN_PATH_SIZE];
    char temp_path[WEB_SCAN_PATH_SIZE];
    char backup_path[WEB_SCAN_PATH_SIZE];
    esp_err_t err = web_scan_paths_for_read(
        object_id,
        session_id,
        &WEB_SCAN_KIND_SCAN,
        final_path,
        temp_path,
        backup_path
    );
    if (err != ESP_OK) return err;

    FILE *file = NULL;
    size_t file_size = 0U;
    err = web_scan_open_for_stream(
        final_path,
        temp_path,
        backup_path,
        &WEB_SCAN_KIND_SCAN,
        &file,
        &file_size
    );
    if (err == ESP_ERR_NOT_FOUND) return err;
    if (err != ESP_OK) return err;

    /* The scan is present and sized even when hashing it is refused below. */
    *present_out = true;
    *size_out = file_size;

    uint32_t crc32 = 0U;
    bool cached = false;
    err = web_scan_identity_cache_lookup(final_path, file_size, &cached, &crc32);
    if (err == ESP_OK && !cached) {
        err = web_scan_crc32_file(file, file_size, &crc32);
    }

    const esp_err_t close_err = web_scan_close_reader(&file);
    if (err == ESP_OK) err = close_err;
    if (err != ESP_OK) return err;

    if (!cached) {
        err = web_scan_identity_cache_store(final_path, file_size, crc32, &file_size, &crc32);
        if (err != ESP_OK) {
            if (err == ESP_ERR_NOT_FOUND) {
                *present_out = false;
                *size_out = 0U;
            }
            return err;
        }
    }
    *present_out = true;
    *size_out = file_size;
    *crc_out = crc32;
    return ESP_OK;
}


/*
 * The SHA-256 of the scan this session reads, for the scan whose CRC32 identity is `expected_crc32`. ESP_ERR_INVALID_STATE means the scan on the
 * card is no longer the one with that CRC (it was replaced while it was hashed).
 */
static esp_err_t web_scan_current_scan_sha256(
    const char *object_id,
    const char *session_id,
    uint32_t expected_crc32,
    unsigned char sha_out[WEB_SCAN_SHA256_BYTES]
)
{
    char final_path[WEB_SCAN_PATH_SIZE];
    char temp_path[WEB_SCAN_PATH_SIZE];
    char backup_path[WEB_SCAN_PATH_SIZE];
    esp_err_t err = web_scan_paths_for_read(object_id, session_id, &WEB_SCAN_KIND_SCAN, final_path, temp_path, backup_path);
    if (err != ESP_OK) return err;

    err = storage_manager_operation_begin();
    if (err != ESP_OK) return err;
    const bool hit = s_scan_sha_cache.valid && strcmp(s_scan_sha_cache.path, final_path) == 0 && s_scan_sha_cache.crc32 == expected_crc32;
    if (hit) memcpy(sha_out, s_scan_sha_cache.sha256, WEB_SCAN_SHA256_BYTES);
    storage_manager_operation_end();
    if (hit) return ESP_OK;

    FILE *file = NULL;
    size_t file_size = 0U;
    err = web_scan_open_for_stream(final_path, temp_path, backup_path, &WEB_SCAN_KIND_SCAN, &file, &file_size);
    if (err != ESP_OK) return err;
    uint32_t crc32 = 0U;
    err = web_scan_hash_file(file, file_size, &crc32, sha_out);
    const esp_err_t close_err = web_scan_close_reader(&file);
    if (err == ESP_OK) err = close_err;
    if (err != ESP_OK) return err;
    if (crc32 != expected_crc32) return ESP_ERR_INVALID_STATE;

    err = storage_manager_operation_begin();
    if (err != ESP_OK) return err;
    /* Remember it only while the identity cache still names this very scan: a replacement in between cleared it, and then there is nothing to keep. */
    if (s_scan_identity_cache.valid && strcmp(s_scan_identity_cache.path, final_path) == 0 && s_scan_identity_cache.crc32 == crc32) {
        memcpy(s_scan_sha_cache.path, final_path, strlen(final_path) + 1U);
        s_scan_sha_cache.crc32 = crc32;
        memcpy(s_scan_sha_cache.sha256, sha_out, WEB_SCAN_SHA256_BYTES);
        s_scan_sha_cache.valid = true;
    }
    storage_manager_operation_end();
    return ESP_OK;
}


static uint32_t web_scan_read_u32_le(const unsigned char bytes[4])
{
    return (uint32_t)bytes[0] |
        ((uint32_t)bytes[1] << 8U) |
        ((uint32_t)bytes[2] << 16U) |
        ((uint32_t)bytes[3] << 24U);
}


static uint16_t web_scan_read_u16_le(const unsigned char bytes[2])
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8U));
}


static bool web_scan_read_exact(FILE *file, unsigned char *bytes, size_t length)
{
    if (file == NULL || bytes == NULL || length == 0U) return false;
    if (storage_manager_operation_begin() != ESP_OK) return false;
    const size_t count = fread(bytes, 1U, length, file);
    const bool read_ok = count == length && ferror(file) == 0;
    storage_manager_operation_end();
    return read_ok;
}


static bool web_scan_is_json_space(char value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}


static bool web_scan_parse_occupancy_frame_id(
    const char *json,
    size_t length,
    uint32_t *crc_out
)
{
    static const char frame_key[] = "\"frame_id\"";
    if (json == NULL || crc_out == NULL || length == 0U) return false;

    size_t first = 0U;
    while (first < length && web_scan_is_json_space(json[first])) ++first;
    size_t last = length;
    while (last > first && web_scan_is_json_space(json[last - 1U])) --last;
    if (first >= last || json[first] != '{' || json[last - 1U] != '}') return false;

    const char *cursor = json + first + 1U;
    const char *limit = json + last;
    while (cursor < limit) {
        const char *key = strstr(cursor, frame_key);
        if (key == NULL || key >= limit) return false;
        const size_t key_offset = (size_t)(key - json);
        size_t boundary = key_offset;
        while (boundary > first && web_scan_is_json_space(json[boundary - 1U])) --boundary;
        if (boundary == first || (json[boundary - 1U] != '{' && json[boundary - 1U] != ',')) {
            cursor = key + 1;
            continue;
        }

        const char *value = key + sizeof(frame_key) - 1U;
        while (value < limit && web_scan_is_json_space(*value)) ++value;
        if (value >= limit || *value++ != ':') return false;
        while (value < limit && web_scan_is_json_space(*value)) ++value;
        if (value >= limit || *value++ != '"') return false;
        if ((size_t)(limit - value) < WEB_SCAN_CRC32_ID_SIZE) return false;

        char id[WEB_SCAN_CRC32_ID_SIZE];
        memcpy(id, value, WEB_SCAN_CRC32_ID_SIZE - 1U);
        id[WEB_SCAN_CRC32_ID_SIZE - 1U] = '\0';
        if (value[WEB_SCAN_CRC32_ID_SIZE - 1U] != '"') return false;
        const char *after = value + WEB_SCAN_CRC32_ID_SIZE;
        while (after < limit && web_scan_is_json_space(*after)) ++after;
        if (after >= limit || (*after != ',' && *after != '}')) return false;
        return web_scan_parse_crc32_id(id, crc_out);
    }
    return false;
}


static bool web_scan_occupancy_source_crc32(FILE *file, size_t file_size, uint32_t *crc_out)
{
    static const unsigned char magic[8] = {'G', 'O', 'C', 'C', 'U', 'P', 'Y', '1'};
    unsigned char fixed_header[20];
    if (file_size < sizeof(fixed_header) || !web_scan_read_exact(file, fixed_header, sizeof(fixed_header))) {
        return false;
    }
    if (memcmp(fixed_header, magic, sizeof(magic)) != 0) return false;

    const uint32_t header_length = web_scan_read_u32_le(fixed_header + 8U);
    const uint32_t payload_length = web_scan_read_u32_le(fixed_header + 12U);
    const uint64_t artifact_size = UINT64_C(20) + (uint64_t)header_length + (uint64_t)payload_length;
    if (
        header_length == 0U || header_length > WEB_SCAN_HEADER_PEEK_MAX_BYTES ||
        artifact_size != (uint64_t)file_size
    ) {
        return false;
    }

    char *header = heap_caps_malloc((size_t)header_length + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (header == NULL) return false;
    const bool read_ok = web_scan_read_exact(file, (unsigned char *)header, (size_t)header_length);
    bool parsed = false;
    if (read_ok && memchr(header, '\0', (size_t)header_length) == NULL) {
        header[header_length] = '\0';
        parsed = web_scan_parse_occupancy_frame_id(header, (size_t)header_length, crc_out);
    }
    free(header);
    return parsed;
}


static bool web_scan_preview_source_crc32(FILE *file, size_t file_size, uint32_t *crc_out)
{
    static const unsigned char magic[8] = {'G', 'X', 'P', 'R', 'V', '1', 0U, 0U};
    unsigned char header[25];
    if (file_size < sizeof(header) || !web_scan_read_exact(file, header, sizeof(header))) return false;
    if (memcmp(header, magic, sizeof(magic)) != 0) return false;

    const uint16_t version = web_scan_read_u16_le(header + 8U);
    const uint16_t header_length = web_scan_read_u16_le(header + 10U);
    const uint8_t registration_flag = header[24U];
    const uint16_t expected_header_length = (uint16_t)(registration_flag != 0U ? 118U : 54U);
    if (
        (version != (uint16_t)1U && version != (uint16_t)2U) || registration_flag > (uint8_t)1U ||
        header_length != expected_header_length ||
        file_size < (size_t)header_length + 4U
    ) {
        return false;
    }

    *crc_out = web_scan_read_u32_le(header + 12U);
    return true;
}


static esp_err_t web_scan_read_bounded(
    FILE *file,
    unsigned char *bytes,
    size_t length,
    bool storage_locked
)
{
    if (file == NULL || bytes == NULL || length == 0U) return ESP_ERR_INVALID_ARG;
    size_t remaining = length;
    while (remaining > 0U) {
        if (web_server_shutdown_requested()) return ESP_ERR_INVALID_STATE;
        const size_t chunk_size = remaining < WEB_SCAN_IO_BUFFER_SIZE
            ? remaining
            : WEB_SCAN_IO_BUFFER_SIZE;
        if (storage_locked) {
            if (fread(bytes, 1U, chunk_size, file) != chunk_size || ferror(file) != 0) {
                return ESP_FAIL;
            }
        }
        else {
            size_t read_count = 0U;
            web_scan_read_chunk_context_t chunk = {
                .file = file,
                .buffer = bytes,
                .capacity = chunk_size,
                .read_out = &read_count,
            };
            const esp_err_t err = storage_bulk_admission_run_chunk(web_scan_read_chunk, &chunk);
            if (err != ESP_OK) return err;
            if (read_count != chunk_size) return ESP_FAIL;
        }
        bytes += chunk_size;
        remaining -= chunk_size;
        if (!storage_locked) taskYIELD();
    }
    return ESP_OK;
}


static esp_err_t web_scan_seek_file(FILE *file, size_t offset, bool storage_locked)
{
    if (file == NULL || offset > (size_t)LONG_MAX) return ESP_ERR_INVALID_ARG;
    if (storage_locked) return fseek(file, (long)offset, SEEK_SET) == 0 ? ESP_OK : ESP_FAIL;

    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) return err;
    const bool seek_ok = fseek(file, (long)offset, SEEK_SET) == 0;
    storage_manager_operation_end();
    return seek_ok ? ESP_OK : ESP_FAIL;
}


static esp_err_t web_scan_crc32_range(
    FILE *file,
    size_t offset,
    size_t length,
    bool storage_locked,
    uint32_t *crc_out
)
{
    if (file == NULL || crc_out == NULL || length == 0U) return ESP_ERR_INVALID_ARG;
    esp_err_t err = web_scan_seek_file(file, offset, storage_locked);
    if (err != ESP_OK) return err;

    unsigned char *buffer = heap_caps_malloc(
        WEB_SCAN_IO_BUFFER_SIZE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (buffer == NULL) return ESP_ERR_NO_MEM;

    size_t remaining = length;
    uint32_t crc = UINT32_C(0xffffffff);
    while (remaining > 0U) {
        if (web_server_shutdown_requested()) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        const size_t request_size = remaining < WEB_SCAN_IO_BUFFER_SIZE
            ? remaining
            : WEB_SCAN_IO_BUFFER_SIZE;
        if (storage_locked) {
            const size_t read_count = fread(buffer, 1U, request_size, file);
            if (read_count != request_size || ferror(file) != 0) {
                err = ESP_FAIL;
                break;
            }
            crc = web_scan_crc32_update(crc, buffer, read_count);
            remaining -= read_count;
        }
        else {
            size_t read_count = 0U;
            web_scan_read_chunk_context_t chunk = {
                .file = file,
                .buffer = buffer,
                .capacity = request_size,
                .read_out = &read_count,
            };
            err = storage_bulk_admission_run_chunk(web_scan_read_chunk, &chunk);
            if (err != ESP_OK || read_count == 0U) {
                if (err == ESP_OK) err = ESP_FAIL;
                break;
            }
            crc = web_scan_crc32_update(crc, buffer, read_count);
            remaining -= read_count;
            taskYIELD();
        }
    }

    free(buffer);
    if (err == ESP_OK) *crc_out = crc ^ UINT32_C(0xffffffff);
    return err;
}


/*
 * registration-v1.json admission: a real parse of the ROOT object, never a substring search (a nested decoy "scan_frame_id" or a repeated
 * root key must not be able to pick the scan binding). Root keys are unique, "schema" is the whole number 1, and "scan_frame_id" is a
 * scan-crc32 string or null. The full field-by-field contract stays with the WebUI decoder and the reader that applies the file.
 */
#define WEB_SCAN_REGISTRATION_MAX_ROOT_KEYS 32

static bool web_scan_parse_registration_json(
    char *json,
    size_t length,
    uint32_t *source_crc_out,
    bool *bound_out
)
{
    if (json == NULL || length == 0U || memchr(json, '\0', length) != NULL) return false;
    json[length] = '\0';
    if (bound_out != NULL) *bound_out = false;

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return false;
    bool ok = cJSON_IsObject(root);

    size_t keys = 0U;
    for (const cJSON *a = ok ? root->child : NULL; ok && a != NULL; a = a->next) {
        if (a->string == NULL || ++keys > WEB_SCAN_REGISTRATION_MAX_ROOT_KEYS) {
            ok = false;
            break;
        }
        for (const cJSON *b = a->next; b != NULL; b = b->next) {
            if (b->string != NULL && strcmp(a->string, b->string) == 0) ok = false; /* duplicate root key */
        }
    }

    const cJSON *schema = ok ? cJSON_GetObjectItemCaseSensitive(root, "schema") : NULL;
    ok = ok && cJSON_IsNumber(schema) && schema->valuedouble == 1.0;

    uint32_t crc32 = 0U;
    bool bound = false;
    const cJSON *frame = ok ? cJSON_GetObjectItemCaseSensitive(root, "scan_frame_id") : NULL;
    if (ok && frame != NULL && !cJSON_IsNull(frame)) {
        ok = cJSON_IsString(frame) && frame->valuestring != NULL &&
            strlen(frame->valuestring) == (size_t)(WEB_SCAN_CRC32_ID_SIZE - 1U) &&
            web_scan_parse_crc32_id(frame->valuestring, &crc32);
        bound = ok;
    }
    cJSON_Delete(root);
    if (!ok) return false;
    if (bound && source_crc_out != NULL) *source_crc_out = crc32;
    if (bound_out != NULL) *bound_out = bound;
    return true;
}


static esp_err_t web_scan_validate_artifact(
    FILE *file,
    size_t file_size,
    const web_scan_kind_t *kind,
    bool storage_locked,
    bool *valid_out,
    uint32_t *source_crc_out,
    bool *registration_bound_out
)
{
    if (file == NULL || kind == NULL || valid_out == NULL) return ESP_ERR_INVALID_ARG;
    *valid_out = false;
    if (source_crc_out != NULL) *source_crc_out = 0U;
    if (registration_bound_out != NULL) *registration_bound_out = false;

    if (kind == &WEB_SCAN_KIND_SCAN) {
        static const unsigned char magic[8] = {'G', 'X', 'R', 'S', 'C', 'A', 'N', '1'};
        unsigned char header[20];
        if (file_size < sizeof(header)) return ESP_OK;
        esp_err_t err = web_scan_seek_file(file, 0U, storage_locked);
        if (err == ESP_OK) err = web_scan_read_bounded(file, header, sizeof(header), storage_locked);
        if (err != ESP_OK) return err;
        if (memcmp(header, magic, sizeof(magic)) != 0) return ESP_OK;
        const uint32_t header_length = web_scan_read_u32_le(header + 8U);
        const uint32_t payload_length = web_scan_read_u32_le(header + 12U);
        const uint64_t expected_size = UINT64_C(20) + (uint64_t)header_length + (uint64_t)payload_length;
        if (header_length == 0U || expected_size != (uint64_t)file_size) return ESP_OK;
        uint32_t actual_crc = 0U;
        err = web_scan_crc32_range(file, 20U, file_size - 20U, storage_locked, &actual_crc);
        if (err != ESP_OK) return err;
        *valid_out = actual_crc == web_scan_read_u32_le(header + 16U);
        return ESP_OK;
    }

    if (kind == &WEB_SCAN_KIND_OCCUPANCY) {
        static const unsigned char magic[8] = {'G', 'O', 'C', 'C', 'U', 'P', 'Y', '1'};
        unsigned char header[20];
        esp_err_t err = web_scan_seek_file(file, 0U, storage_locked);
        if (file_size < sizeof(header)) return ESP_OK;
        if (err == ESP_OK) err = web_scan_read_bounded(file, header, sizeof(header), storage_locked);
        if (err != ESP_OK) return err;
        if (memcmp(header, magic, sizeof(magic)) != 0) return ESP_OK;
        const uint32_t json_length = web_scan_read_u32_le(header + 8U);
        const uint32_t payload_length = web_scan_read_u32_le(header + 12U);
        const uint64_t expected_size = UINT64_C(20) + (uint64_t)json_length + (uint64_t)payload_length;
        if (
            json_length == 0U || json_length > WEB_SCAN_HEADER_PEEK_MAX_BYTES ||
            expected_size != (uint64_t)file_size
        ) return ESP_OK;

        uint32_t actual_crc = 0U;
        err = web_scan_crc32_range(file, 20U, file_size - 20U, storage_locked, &actual_crc);
        if (err != ESP_OK) return err;
        if (actual_crc != web_scan_read_u32_le(header + 16U)) return ESP_OK;

        char *json = heap_caps_malloc((size_t)json_length + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (json == NULL) return ESP_ERR_NO_MEM;
        err = web_scan_seek_file(file, 20U, storage_locked);
        if (err == ESP_OK) err = web_scan_read_bounded(file, (unsigned char *)json, (size_t)json_length, storage_locked);
        uint32_t source_crc32 = 0U;
        const bool parsed = err == ESP_OK && memchr(json, '\0', (size_t)json_length) == NULL &&
            (json[json_length] = '\0', web_scan_parse_occupancy_frame_id(json, (size_t)json_length, &source_crc32));
        free(json);
        if (err != ESP_OK) return err;
        *valid_out = parsed;
        if (parsed && source_crc_out != NULL) *source_crc_out = source_crc32;
        return ESP_OK;
    }

    if (kind == &WEB_SCAN_KIND_PREVIEW) {
        static const unsigned char magic[8] = {'G', 'X', 'P', 'R', 'V', '1', 0U, 0U};
        unsigned char header[118];
        esp_err_t err = web_scan_seek_file(file, 0U, storage_locked);
        if (file_size < 58U) return ESP_OK;
        if (err == ESP_OK) err = web_scan_read_bounded(file, header, 54U, storage_locked);
        if (err != ESP_OK) return err;
        if (memcmp(header, magic, sizeof(magic)) != 0) return ESP_OK;
        const uint16_t version = web_scan_read_u16_le(header + 8U);
        const uint16_t header_length = web_scan_read_u16_le(header + 10U);
        const uint8_t registration_flag = header[24U];
        const uint16_t expected_header_length = registration_flag != 0U ? 118U : 54U;
        const uint8_t index_width = header[25U];
        const uint16_t dim_x = web_scan_read_u16_le(header + 26U);
        const uint16_t dim_y = web_scan_read_u16_le(header + 28U);
        const uint16_t dim_z = web_scan_read_u16_le(header + 30U);
        const uint32_t voxel_count = web_scan_read_u32_le(header + 48U);
        const uint64_t volume = (uint64_t)dim_x * (uint64_t)dim_y * (uint64_t)dim_z;
        const uint64_t expected_size = (uint64_t)header_length +
            (uint64_t)voxel_count * ((uint64_t)index_width + 1U) + UINT64_C(4);
        if (
            (version != 1U && version != 2U) || registration_flag > 1U ||
            header_length != expected_header_length ||
            (index_width != 2U && index_width != 4U) ||
            dim_x == 0U || dim_y == 0U || dim_z == 0U ||
            volume == 0U || (uint64_t)voxel_count > volume ||
            expected_size != (uint64_t)file_size
        ) return ESP_OK;
        if (registration_flag != 0U) {
            err = web_scan_seek_file(file, 0U, storage_locked);
            if (err == ESP_OK) err = web_scan_read_bounded(file, header, expected_header_length, storage_locked);
            if (err != ESP_OK) return err;
        }

        unsigned char crc_bytes[4];
        err = web_scan_seek_file(file, file_size - sizeof(crc_bytes), storage_locked);
        if (err == ESP_OK) err = web_scan_read_bounded(file, crc_bytes, sizeof(crc_bytes), storage_locked);
        if (err != ESP_OK) return err;
        uint32_t actual_crc = 0U;
        err = web_scan_crc32_range(file, 0U, file_size - sizeof(crc_bytes), storage_locked, &actual_crc);
        if (err != ESP_OK) return err;
        *valid_out = actual_crc == web_scan_read_u32_le(crc_bytes);
        if (*valid_out && source_crc_out != NULL) *source_crc_out = web_scan_read_u32_le(header + 12U);
        return ESP_OK;
    }

    if (web_scan_kind_is_bound_json(kind)) {
        if (file_size == 0U || file_size > kind->max_bytes) return ESP_OK;
        char *json = heap_caps_malloc(file_size + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (json == NULL) return ESP_ERR_NO_MEM;
        esp_err_t err = web_scan_seek_file(file, 0U, storage_locked);
        if (err == ESP_OK) err = web_scan_read_bounded(file, (unsigned char *)json, file_size, storage_locked);
        uint32_t source_crc32 = 0U;
        bool registration_bound = false;
        bool parsed = err == ESP_OK && web_scan_parse_registration_json(
            json,
            file_size,
            &source_crc32,
            &registration_bound
        );
        /* The tag positions are held to the contract the localization reads them under: a file it would ignore is refused here, to the phone. */
        if (parsed && kind == &WEB_SCAN_KIND_TAGS) {
            seismic_recorder_tag_layout_t layout;
            parsed = seismic_recorder_tag_layout_parse(json, &layout) == ESP_OK;
        }
        free(json);
        if (err != ESP_OK) return err;
        *valid_out = parsed;
        if (parsed && registration_bound_out != NULL) *registration_bound_out = registration_bound;
        if (parsed && registration_bound && source_crc_out != NULL) *source_crc_out = source_crc32;
        return ESP_OK;
    }

    return ESP_ERR_INVALID_ARG;
}


static esp_err_t web_scan_validate_path_locked(
    const char *path,
    const web_scan_kind_t *kind,
    bool *valid_out,
    uint32_t *source_crc_out,
    bool *registration_bound_out
)
{
    if (path == NULL || kind == NULL || valid_out == NULL) return ESP_ERR_INVALID_ARG;
    *valid_out = false;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return ESP_ERR_NOT_FOUND;
    esp_err_t err = ESP_OK;
    if (fseek(file, 0L, SEEK_END) != 0) err = ESP_FAIL;
    const long size = err == ESP_OK ? ftell(file) : -1L;
    if (err == ESP_OK && (size <= 0L || (unsigned long)size > kind->max_bytes)) {
        (void)fclose(file);
        return ESP_OK;
    }
    if (err == ESP_OK && fseek(file, 0L, SEEK_SET) != 0) err = ESP_FAIL;
    if (err == ESP_OK) {
        err = web_scan_validate_artifact(
            file,
            (size_t)size,
            kind,
            true,
            valid_out,
            source_crc_out,
            registration_bound_out
        );
    }
    if (fclose(file) != 0 && err == ESP_OK) err = ESP_FAIL;
    return err;
}


static esp_err_t web_scan_validate_uploaded_temp(
    const char *temp_path,
    const web_scan_kind_t *kind,
    uint32_t *source_crc_out,
    bool *registration_bound_out,
    bool *valid_out
)
{
    if (temp_path == NULL || kind == NULL || valid_out == NULL) return ESP_ERR_INVALID_ARG;
    *valid_out = false;
    const bool registration = web_scan_kind_is_bound_json(kind);
    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) return err;
    FILE *file = fopen(temp_path, "rb");
    if (file == NULL) err = errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    if (err == ESP_OK && fseek(file, 0L, SEEK_END) != 0) err = ESP_FAIL;
    const long file_size = err == ESP_OK ? ftell(file) : -1L;
    if (err == ESP_OK && !web_scan_kind_size_valid(kind, file_size)) err = ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK && fseek(file, 0L, SEEK_SET) != 0) err = ESP_FAIL;

    if (registration && err == ESP_OK) {
        err = web_scan_validate_artifact(
            file,
            (size_t)file_size,
            kind,
            true,
            valid_out,
            source_crc_out,
            registration_bound_out
        );
        if (fclose(file) != 0 && err == ESP_OK) err = ESP_FAIL;
        file = NULL;
        storage_manager_operation_end();
    }
    else {
        storage_manager_operation_end();
    }

    if (err == ESP_OK && !registration) {
        err = web_scan_validate_artifact(
            file,
            (size_t)file_size,
            kind,
            false,
            valid_out,
            source_crc_out,
            registration_bound_out
        );
    }
    if (file != NULL) {
        const esp_err_t close_err = web_scan_close_reader(&file);
        if (err == ESP_OK) err = close_err;
    }
    return err;
}


static esp_err_t web_scan_load(httpd_req_t *request, const char *object_id, const char *session_id, const web_scan_kind_t *kind)
{
    if (
        web_scan_requires_bulk_admission(kind, session_id) &&
        storage_bulk_admission_is_protected()
    ) {
        return web_scan_send_recording_busy(request, "scan_load");
    }

    esp_err_t err = web_scan_verify_object(object_id);
    if (err == ESP_OK) err = web_scan_verify_session(object_id, session_id);
    if (err != ESP_OK) return web_scan_send_json(request, "scan_load", object_id, session_id, err, 0U);

    char final_path[WEB_SCAN_PATH_SIZE], temp_path[WEB_SCAN_PATH_SIZE], backup_path[WEB_SCAN_PATH_SIZE];
    err = web_scan_paths_for_read(object_id, session_id, kind, final_path, temp_path, backup_path);
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
    httpd_resp_set_type(request, web_scan_kind_is_bound_json(kind)
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
        err = web_scan_requires_bulk_admission(kind, session_id)
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
        web_scan_requires_bulk_admission(kind, session_id) &&
        storage_bulk_admission_is_protected()
    ) {
        return web_scan_send_recording_busy(request, "scan_delete");
    }

    esp_err_t err = web_scan_verify_object(object_id);
    if (err == ESP_OK) err = web_scan_verify_session(object_id, session_id);
    if (err != ESP_OK) return web_scan_send_json(request, "scan_delete", object_id, session_id, err, 0U);
    if (
        web_scan_kind_is_bound_json(kind) && !web_scan_session_is_staging(session_id) &&
        web_scan_registration_session_is_recording(object_id, session_id)
    ) {
        return web_scan_send_registration_conflict(request);
    }

    char final_path[WEB_SCAN_PATH_SIZE], temp_path[WEB_SCAN_PATH_SIZE], backup_path[WEB_SCAN_PATH_SIZE];
    err = web_scan_paths(object_id, session_id, kind, final_path, temp_path, backup_path);
    if (err != ESP_OK) return web_scan_send_json(request, "scan_delete", object_id, session_id, err, 0U);

    err = storage_manager_operation_begin();
    if (err != ESP_OK) return web_scan_send_json(request, "scan_delete", object_id, session_id, err, 0U);
    if (kind == &WEB_SCAN_KIND_SCAN) {
        web_scan_identity_cache_clear_locked(final_path);
        /* Remove registration before the scan so a failed cascade cannot leave
         * a registration that a later scan could inherit. */
        err = web_scan_remove_kind_locked(object_id, session_id, &WEB_SCAN_KIND_REGISTRATION);
        if (err == ESP_OK) err = web_scan_remove_kind_locked(object_id, session_id, &WEB_SCAN_KIND_TAGS);
        if (err == ESP_OK) {
            err = web_scan_remove_kind_locked(object_id, session_id, &WEB_SCAN_KIND_OCCUPANCY);
        }
        if (err == ESP_OK) {
            err = web_scan_remove_kind_locked(object_id, session_id, &WEB_SCAN_KIND_PREVIEW);
        }
        if (err == ESP_OK) err = web_scan_remove_paths_locked(final_path, temp_path, backup_path);
        /*
         * A recorded session usually holds no scan bytes of its own, only a scan-ref to the shared scans/SCnnnnnn. "Delete the scan of this
         * recording" means: unlink THIS session's reference (issue #24). The shared scan stays for the other sessions that use it; without
         * this the delete answered ok and the shared scan resolved again on the next read.
         */
        if (err == ESP_OK && !web_scan_session_is_staging(session_id)) {
            char ref_path[WEB_SCAN_PATH_SIZE];
            if (scan_artifact_session_path(STORAGE_MANAGER_OBJECTS_PATH, object_id, session_id, SCAN_ARTIFACT_REF_FILE, ref_path, sizeof(ref_path)) != 0) {
                err = ESP_ERR_INVALID_SIZE;
            } else if (remove(ref_path) != 0 && errno != ENOENT) {
                err = ESP_FAIL;
            }
        }
    }
    else err = web_scan_remove_paths_locked(final_path, temp_path, backup_path);
    storage_manager_operation_end();

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Deleted saved %s: object=%s session=%s", kind->label, object_id, session_id);
    }
    return web_scan_send_json(request, "scan_delete", object_id, session_id, err, 0U);
}


/* MAX-169: report artifact presence/size without streaming it over the ESP link. */
static esp_err_t web_scan_info(httpd_req_t *request, const char *object_id, const char *session_id, const web_scan_kind_t *kind)
{
    if (
        web_scan_session_is_staging(session_id) &&
        storage_bulk_admission_is_protected()
    ) {
        return web_scan_send_recording_busy(request, "scan_info");
    }

    esp_err_t err = web_scan_verify_object(object_id);
    if (err == ESP_OK) err = web_scan_verify_session(object_id, session_id);
    if (err != ESP_OK) return web_scan_send_json(request, "scan_info", object_id, session_id, err, 0U);

    if (kind == &WEB_SCAN_KIND_SCAN) {
        bool present = false;
        size_t size = 0U;
        uint32_t crc32 = 0U;
        err = web_scan_get_scan_identity(object_id, session_id, &present, &size, &crc32);
        if (err == ESP_ERR_INVALID_STATE && present) {
            /* Bulk admission is protected (recording) or the server is shutting down: the
             * scan is there and sized, but hashing it now would stall the recorder, so its
             * identity is reported as unknown (null) instead of failing the info request. */
            return web_scan_send_json_extra(
                request,
                "scan_info",
                object_id,
                session_id,
                ESP_OK,
                size,
                "id",
                NULL,
                true
            );
        }
        if (err != ESP_OK || !present) {
            return web_scan_send_json_extra(
                request,
                "scan_info",
                object_id,
                session_id,
                err == ESP_OK ? ESP_ERR_NOT_FOUND : err,
                0U,
                "id",
                NULL,
                true
            );
        }
        char scan_id[WEB_SCAN_CRC32_ID_SIZE];
        web_scan_format_crc32_id(crc32, scan_id);
        return web_scan_send_json_extra(
            request,
            "scan_info",
            object_id,
            session_id,
            ESP_OK,
            size,
            "id",
            scan_id,
            false
        );
    }

    char final_path[WEB_SCAN_PATH_SIZE], temp_path[WEB_SCAN_PATH_SIZE], backup_path[WEB_SCAN_PATH_SIZE];
    err = web_scan_paths_for_read(object_id, session_id, kind, final_path, temp_path, backup_path);
    if (err != ESP_OK) {
        if (kind == &WEB_SCAN_KIND_OCCUPANCY || kind == &WEB_SCAN_KIND_PREVIEW) {
            return web_scan_send_json_extra(request, "scan_info", object_id, session_id, err, 0U, "source_id", NULL, true);
        }
        return web_scan_send_json(request, "scan_info", object_id, session_id, err, 0U);
    }

    FILE *file = NULL;
    size_t size = 0U;
    err = web_scan_open_for_stream(final_path, temp_path, backup_path, kind, &file, &size);
    bool source_id_present = false;
    char source_id[WEB_SCAN_CRC32_ID_SIZE] = {0};
    if (err == ESP_OK) {
        uint32_t source_crc32 = 0U;
        if (kind == &WEB_SCAN_KIND_OCCUPANCY) {
            source_id_present = web_scan_occupancy_source_crc32(file, size, &source_crc32);
        }
        else if (kind == &WEB_SCAN_KIND_PREVIEW) {
            source_id_present = web_scan_preview_source_crc32(file, size, &source_crc32);
        }
        const esp_err_t close_err = web_scan_close_reader(&file);
        if (close_err != ESP_OK) err = close_err;
        if (source_id_present) web_scan_format_crc32_id(source_crc32, source_id);
    }
    if (kind == &WEB_SCAN_KIND_OCCUPANCY || kind == &WEB_SCAN_KIND_PREVIEW) {
        return web_scan_send_json_extra(
            request,
            "scan_info",
            object_id,
            session_id,
            err,
            err == ESP_OK ? size : 0U,
            "source_id",
            source_id_present ? source_id : NULL,
            !source_id_present
        );
    }
    return web_scan_send_json(request, "scan_info", object_id, session_id, err, err == ESP_OK ? size : 0U);
}


esp_err_t web_scan_store_clear_staging(const char *object_id)
{
    if (!object_format_id_is_valid(object_id)) return ESP_ERR_INVALID_ARG;

    esp_err_t err = web_scan_verify_object(object_id);
    if (err != ESP_OK) return err;

    err = storage_manager_operation_begin();
    if (err != ESP_OK) return err;

    const web_scan_kind_t *const kinds[] = {
        &WEB_SCAN_KIND_SCAN,
        &WEB_SCAN_KIND_OCCUPANCY,
        &WEB_SCAN_KIND_PREVIEW,
        &WEB_SCAN_KIND_REGISTRATION,
        &WEB_SCAN_KIND_TAGS,
    };
    for (size_t i = 0U; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
        char final_path[WEB_SCAN_PATH_SIZE], temp_path[WEB_SCAN_PATH_SIZE], backup_path[WEB_SCAN_PATH_SIZE];
        err = web_scan_paths(object_id, "staging", kinds[i], final_path, temp_path, backup_path);
        if (err != ESP_OK) break;
        if (kinds[i] == &WEB_SCAN_KIND_SCAN) web_scan_identity_cache_clear_locked(final_path);
        const char *const paths[] = {final_path, temp_path, backup_path};
        for (size_t path_index = 0U; path_index < sizeof(paths) / sizeof(paths[0]); ++path_index) {
            if (remove(paths[path_index]) != 0 && errno != ENOENT) err = ESP_FAIL;
        }
        if (err != ESP_OK) break;
    }

    storage_manager_operation_end();
    return err;
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
        } else if (strcmp(kind_value, "tags") == 0) {
            kind = &WEB_SCAN_KIND_TAGS;
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
