#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <sys/stat.h>

#include "app_task_config.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "object_format.h"
#include "object_store.h"
#include "seismic_recorder.h"
#include "storage_bulk_admission.h"
#include "storage_manager_internal.h"
#include "storage_manager.h"
#include "web_server_internal.h"

typedef struct {
    bool locked;
} fake_mutex_t;

static unsigned s_storage_begin_calls;
static unsigned s_storage_end_calls;
static unsigned s_priority_changes;
static size_t s_chunked_response_bytes;
static unsigned s_session_list_calls;
static unsigned s_recv_calls;
static unsigned s_enter_after_recv_call;
static unsigned s_send_chunk_calls;
static unsigned s_enter_after_send_chunk;
static unsigned s_fail_fwrite_call;
static unsigned s_fwrite_calls;
static const char *s_fail_remove_path;
static const char *s_cut_after_remove_path;
static const char *s_cut_after_rename_path;
static bool s_replace_scan_before_derivative_commit;
static bool s_replace_scan_before_identity_cache_store;
static bool s_remove_scan_before_identity_cache_store;
static bool s_storage_power_lost;
static bool s_chunked_response_terminated;
static seismic_recorder_snapshot_t s_recorder_snapshot;

static void reset_web_scan_test_state(void);
static bool save_kind(const char *query, const unsigned char *body, size_t length);
#define WEB_SCAN_STORE_TESTING
void web_scan_store_test_before_derivative_commit(void);
void web_scan_store_test_before_identity_cache_store(void);

size_t web_scan_store_test_fwrite(
    const void *buffer,
    size_t size,
    size_t count,
    FILE *file
);

int web_scan_store_test_remove(const char *path);
int web_scan_store_test_rename(const char *old_path, const char *new_path);

static void make_directory(const char *path)
{
    assert(mkdir(path, 0775) == 0 || errno == EEXIST);
}

static void make_layout(void)
{
    reset_web_scan_test_state();
    make_directory(STORAGE_MANAGER_MOUNT_POINT);
    make_directory(STORAGE_MANAGER_ROOT_PATH);
    make_directory(STORAGE_MANAGER_OBJECTS_PATH);
    make_directory(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001");
    make_directory(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions");
    make_directory(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001");
}

static void remove_layout(void)
{
    const char *final_path =
        STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/scan-v1.bin";
    const char *temp_path =
        STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/scan-v1.bin.tmp";
    const char *backup_path =
        STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/scan-v1.bin.bak";
    const char *registration_path =
        STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/registration-v1.json";
    const char *registration_temp_path =
        STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/registration-v1.json.tmp";
    const char *registration_backup_path =
        STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/registration-v1.json.bak";
    const char *occupancy_path =
        STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/occupancy-v1.bin";
    const char *occupancy_temp_path =
        STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/occupancy-v1.bin.tmp";
    const char *occupancy_backup_path =
        STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/occupancy-v1.bin.bak";
    const char *preview_path =
        STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/preview-v1.bin";
    const char *preview_temp_path =
        STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/preview-v1.bin.tmp";
    const char *preview_backup_path =
        STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/preview-v1.bin.bak";
    (void)remove(final_path);
    (void)remove(temp_path);
    (void)remove(backup_path);
    (void)remove(registration_path);
    (void)remove(registration_temp_path);
    (void)remove(registration_backup_path);
    (void)remove(occupancy_path);
    (void)remove(occupancy_temp_path);
    (void)remove(occupancy_backup_path);
    (void)remove(preview_path);
    (void)remove(preview_temp_path);
    (void)remove(preview_backup_path);
    static const char *const staged_names[] = {
        "scan-v1.bin", "occupancy-v1.bin", "preview-v1.bin", "registration-v1.json",
    };
    static const char *const staged_suffixes[] = {"", ".tmp", ".bak"};
    for (size_t i = 0U; i < sizeof(staged_names) / sizeof(staged_names[0]); ++i) {
        for (size_t suffix = 0U; suffix < sizeof(staged_suffixes) / sizeof(staged_suffixes[0]); ++suffix) {
            char path[256];
            (void)snprintf(
                path,
                sizeof(path),
                STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/staging/%s%s",
                staged_names[i],
                staged_suffixes[suffix]
            );
            (void)remove(path);
        }
    }
#ifdef _WIN32
    (void)_rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001");
    (void)_rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions");
    (void)_rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/staging");
    (void)_rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001");
    (void)_rmdir(STORAGE_MANAGER_OBJECTS_PATH);
    (void)_rmdir(STORAGE_MANAGER_ROOT_PATH);
    (void)_rmdir(STORAGE_MANAGER_MOUNT_POINT);
#else
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001");
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions");
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/staging");
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001");
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH);
    (void)rmdir(STORAGE_MANAGER_ROOT_PATH);
    (void)rmdir(STORAGE_MANAGER_MOUNT_POINT);
#endif
    memset(&s_recorder_snapshot, 0, sizeof(s_recorder_snapshot));
    reset_web_scan_test_state();
}


static bool path_exists(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0;
}


static void write_bytes(const char *path, const unsigned char *bytes, size_t length)
{
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(bytes, 1U, length, file) == length);
    assert(fclose(file) == 0);
}


size_t web_scan_store_test_fwrite(
    const void *buffer,
    size_t size,
    size_t count,
    FILE *file
)
{
    ++s_fwrite_calls;
    if (s_fail_fwrite_call != 0U && s_fwrite_calls == s_fail_fwrite_call) {
        return 0U;
    }
    return fwrite(buffer, size, count, file);
}


int web_scan_store_test_remove(const char *path)
{
    if (s_storage_power_lost) {
        errno = EIO;
        return -1;
    }
    if (s_fail_remove_path != NULL && strcmp(path, s_fail_remove_path) == 0) {
        errno = EACCES;
        return -1;
    }
    const int result = remove(path);
    if (s_cut_after_remove_path != NULL && strcmp(path, s_cut_after_remove_path) == 0) {
        s_cut_after_remove_path = NULL;
        s_storage_power_lost = true;
    }
    return result;
}


int web_scan_store_test_rename(const char *old_path, const char *new_path)
{
    if (s_storage_power_lost) {
        errno = EIO;
        return -1;
    }
    const int result = rename(old_path, new_path);
    if (
        result == 0 && s_cut_after_rename_path != NULL &&
        strcmp(new_path, s_cut_after_rename_path) == 0
    ) {
        s_cut_after_rename_path = NULL;
        s_storage_power_lost = true;
    }
    return result;
}


SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    fake_mutex_t *mutex = calloc(1U, sizeof(*mutex));
    assert(mutex != NULL);
    return mutex;
}


BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout)
{
    fake_mutex_t *mutex = semaphore;
    assert(mutex != NULL);
    assert(timeout == portMAX_DELAY);
    if (mutex->locked) return pdFALSE;
    mutex->locked = true;
    return pdTRUE;
}


BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    fake_mutex_t *mutex = semaphore;
    assert(mutex != NULL && mutex->locked);
    mutex->locked = false;
    return pdTRUE;
}

bool power_control_is_shutdown_requested(void)
{
    return false;
}

esp_err_t storage_manager_operation_begin(void)
{
    s_storage_begin_calls++;
    if (s_storage_power_lost) return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

void storage_manager_operation_end(void)
{
    s_storage_end_calls++;
}

esp_err_t storage_manager_create_directory(const char *path)
{
    if (path == NULL) return ESP_ERR_INVALID_ARG;
    const esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) return err;
    const bool created = mkdir(path, 0775) == 0 || errno == EEXIST;
    storage_manager_operation_end();
    return created ? ESP_OK : ESP_FAIL;
}

esp_err_t object_store_get_profile(
    const char *object_id,
    object_store_profile_t *profile
)
{
    if (object_id == NULL || profile == NULL) return ESP_ERR_INVALID_ARG;
    memset(profile, 0, sizeof(*profile));
    return ESP_OK;
}

esp_err_t seismic_recorder_list_sessions(
    const char *object_id,
    seismic_recorder_session_summary_t *sessions,
    size_t max_sessions,
    size_t *session_count
)
{
    ++s_session_list_calls;
    if (
        object_id == NULL ||
        sessions == NULL ||
        max_sessions == 0U ||
        session_count == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(sessions, 0, sizeof(*sessions));
    (void)snprintf(sessions[0].session_id, sizeof(sessions[0].session_id), "S000001");
    *session_count = 1U;
    return ESP_OK;
}

esp_err_t seismic_recorder_get_snapshot(seismic_recorder_snapshot_t *snapshot)
{
    if (snapshot == NULL) return ESP_ERR_INVALID_ARG;
    *snapshot = s_recorder_snapshot;
    return ESP_OK;
}

void *heap_caps_malloc(size_t size, unsigned capabilities)
{
    (void)capabilities;
    return malloc(size);
}

int64_t esp_timer_get_time(void)
{
    static int64_t now;
    now += 1000;
    return now;
}

UBaseType_t uxTaskPriorityGet(void *task)
{
    (void)task;
    return 5U;
}

void vTaskPrioritySet(void *task, UBaseType_t priority)
{
    (void)task;
    (void)priority;
    s_priority_changes++;
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

esp_err_t httpd_query_key_value(
    const char *query,
    const char *key,
    char *value,
    size_t value_size
)
{
    return query_value(query, key, value, value_size);
}

int httpd_req_recv(httpd_req_t *request, void *buffer, size_t length)
{
    if (request == NULL || buffer == NULL || request->body == NULL) return ESP_FAIL;
    ++s_recv_calls;
    const size_t total = (size_t)request->content_len;
    if (request->body_offset >= total) return 0;
    const size_t remaining = total - request->body_offset;
    const size_t copied = remaining < length ? remaining : length;
    memcpy(buffer, request->body + request->body_offset, copied);
    request->body_offset += copied;
    if (s_enter_after_recv_call != 0U && s_recv_calls == s_enter_after_recv_call) {
        s_enter_after_recv_call = 0U;
        assert(storage_bulk_admission_enter_protected() == ESP_OK);
    }
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
    if (strcmp(field, "X-Geo-Scan-Bytes") == 0) request->scan_bytes_header = value;
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
    request->response_status = status == HTTPD_400_BAD_REQUEST
        ? "400 Bad Request"
        : "500 Internal Server Error";
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
    if (buffer == NULL && length == 0) {
        s_chunked_response_terminated = true;
        return ESP_OK;
    }
    assert(request != NULL && buffer != NULL && length > 0);
    assert(request->response_length + (size_t)length <= request->response_capacity);
    memcpy(request->response_body + request->response_length, buffer, (size_t)length);
    request->response_length += (size_t)length;
    s_chunked_response_bytes += (size_t)length;
    ++s_send_chunk_calls;
    if (s_enter_after_send_chunk != 0U && s_send_chunk_calls == s_enter_after_send_chunk) {
        s_enter_after_send_chunk = 0U;
        assert(storage_bulk_admission_enter_protected() == ESP_OK);
    }
    return ESP_OK;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *uri)
{
    (void)server;
    (void)uri;
    return ESP_OK;
}

bool object_format_id_is_valid(const char *object_id)
{
    return object_id != NULL && strcmp(object_id, "OBJ_0001") == 0;
}

#define static
#define fwrite web_scan_store_test_fwrite
#define remove web_scan_store_test_remove
#define rename web_scan_store_test_rename
#include "../../components/web_server/web_scan_store.c"
#undef rename
#undef remove
#undef fwrite
#undef static

static void reset_web_scan_test_state(void)
{
    s_fail_remove_path = NULL;
    s_cut_after_remove_path = NULL;
    s_cut_after_rename_path = NULL;
    s_replace_scan_before_derivative_commit = false;
    s_replace_scan_before_identity_cache_store = false;
    s_remove_scan_before_identity_cache_store = false;
    s_storage_power_lost = false;
    s_scan_identity_cache.valid = false;
}

static httpd_req_t make_request(
    const char *query,
    const unsigned char *body,
    size_t body_length,
    char *response,
    size_t response_capacity
)
{
    return (httpd_req_t){
        .uri = "/api/control",
        .query = query,
        .body = (const char *)body,
        .content_len = (int)body_length,
        .response_body = response,
        .response_capacity = response_capacity,
    };
}


static uint32_t reference_crc32(const unsigned char *bytes, size_t length)
{
    uint32_t crc = UINT32_C(0xffffffff);
    for (size_t i = 0U; i < length; ++i) {
        crc ^= (uint32_t)bytes[i];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return crc ^ UINT32_C(0xffffffff);
}


void web_scan_store_test_before_derivative_commit(void)
{
    if (!s_replace_scan_before_derivative_commit) return;
    s_replace_scan_before_derivative_commit = false;
    static const unsigned char replacement_scan[] = {0x71U, 0x72U, 0x73U, 0x74U};
    const char *scan_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/scan-v1.bin";
    write_bytes(scan_path, replacement_scan, sizeof(replacement_scan));
    assert(s_scan_identity_cache.valid);
    s_scan_identity_cache.size = sizeof(replacement_scan);
    s_scan_identity_cache.crc32 = reference_crc32(replacement_scan, sizeof(replacement_scan));
}


void web_scan_store_test_before_identity_cache_store(void)
{
    if (s_remove_scan_before_identity_cache_store) {
        s_remove_scan_before_identity_cache_store = false;
        (void)remove(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/scan-v1.bin");
        return;
    }
    if (!s_replace_scan_before_identity_cache_store) return;
    s_replace_scan_before_identity_cache_store = false;
    static const unsigned char replacement_scan[] = {0x71U, 0x72U, 0x73U, 0x74U};
    const char *scan_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/scan-v1.bin";
    write_bytes(scan_path, replacement_scan, sizeof(replacement_scan));
    web_scan_identity_cache_set_locked(
        scan_path,
        sizeof(replacement_scan),
        reference_crc32(replacement_scan, sizeof(replacement_scan))
    );
}


static void make_scan_id(uint32_t crc32, char id[20])
{
    const int written = snprintf(id, 20U, "scan-crc32:%08x", (unsigned)crc32);
    assert(written == 19);
}


static void write_u16_le(unsigned char *bytes, uint16_t value)
{
    bytes[0] = (unsigned char)(value & 0xffU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xffU);
}


static void write_u32_le(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)(value & 0xffU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xffU);
    bytes[2] = (unsigned char)((value >> 16U) & 0xffU);
    bytes[3] = (unsigned char)((value >> 24U) & 0xffU);
}


static size_t make_occupancy_header(unsigned char *artifact, size_t capacity, const char *frame_id)
{
    char json[256];
    const int written = snprintf(
        json,
        sizeof(json),
        "{\"schema\":2,\"frame_id\":\"%s\",\"voxel_size_m\":0.1,\"below_surface_m\":0,\"free_count\":0,\"occupied_count\":0,\"created_at_ms\":1}",
        frame_id
    );
    assert(written > 0 && (size_t)written < sizeof(json));
    const size_t json_length = (size_t)written;
    const size_t total = 20U + json_length;
    assert(artifact != NULL && capacity >= total);
    memset(artifact, 0, total);
    memcpy(artifact, "GOCCUPY1", 8U);
    write_u32_le(artifact + 8U, (uint32_t)json_length);
    write_u32_le(artifact + 12U, 0U);
    write_u32_le(artifact + 16U, reference_crc32((const unsigned char *)json, json_length));
    memcpy(artifact + 20U, json, json_length);
    return total;
}


static size_t make_large_occupancy(
    unsigned char *artifact,
    size_t capacity,
    size_t total_size,
    const char *frame_id
)
{
    char json[256];
    const int written = snprintf(
        json,
        sizeof(json),
        "{\"schema\":2,\"frame_id\":\"%s\",\"voxel_size_m\":0.1}",
        frame_id
    );
    assert(written > 0 && (size_t)written < sizeof(json));
    const size_t json_length = (size_t)written;
    assert(total_size >= 20U + json_length && total_size <= capacity);
    const size_t payload_length = total_size - 20U - json_length;
    memset(artifact, 0, total_size);
    memcpy(artifact, "GOCCUPY1", 8U);
    write_u32_le(artifact + 8U, (uint32_t)json_length);
    write_u32_le(artifact + 12U, (uint32_t)payload_length);
    memcpy(artifact + 20U, json, json_length);
    write_u32_le(artifact + 16U, reference_crc32(artifact + 20U, total_size - 20U));
    return total_size;
}


static size_t make_preview_header(unsigned char *artifact, size_t capacity, uint32_t source_crc32)
{
    const size_t total = 58U;
    assert(artifact != NULL && capacity >= total);
    memset(artifact, 0, total);
    memcpy(artifact, "GXPRV1\0\0", 8U);
    write_u16_le(artifact + 8U, 2U);
    write_u16_le(artifact + 10U, 54U);
    write_u32_le(artifact + 12U, source_crc32);
    artifact[24U] = 0U;
    artifact[25U] = 2U;
    write_u16_le(artifact + 26U, 1U);
    write_u16_le(artifact + 28U, 1U);
    write_u16_le(artifact + 30U, 1U);
    write_u32_le(artifact + 44U, UINT32_C(0x3f800000));
    write_u32_le(artifact + total - 4U, reference_crc32(artifact, total - 4U));
    return total;
}


static size_t make_large_preview(
    unsigned char *artifact,
    size_t capacity,
    size_t total_size,
    uint32_t source_crc32
)
{
    assert(total_size >= 58U && total_size <= capacity);
    const size_t payload_size = total_size - 58U;
    assert(payload_size % 3U == 0U);
    const size_t voxel_count = payload_size / 3U;
    assert(voxel_count <= UINT32_MAX && voxel_count <= UINT16_MAX);
    memset(artifact, 0, total_size);
    memcpy(artifact, "GXPRV1\0\0", 8U);
    write_u16_le(artifact + 8U, 2U);
    write_u16_le(artifact + 10U, 54U);
    write_u32_le(artifact + 12U, source_crc32);
    artifact[24U] = 0U;
    artifact[25U] = 2U;
    write_u16_le(artifact + 26U, (uint16_t)voxel_count);
    write_u16_le(artifact + 28U, 1U);
    write_u16_le(artifact + 30U, 1U);
    write_u32_le(artifact + 44U, UINT32_C(0x3f800000));
    write_u32_le(artifact + 48U, (uint32_t)voxel_count);
    write_u32_le(artifact + total_size - 4U, reference_crc32(artifact, total_size - 4U));
    return total_size;
}


static size_t make_scan_artifact(unsigned char *artifact, size_t capacity, unsigned char marker)
{
    static const unsigned char header_json[] = "{}";
    const size_t total = 20U + sizeof(header_json) - 1U + 1U;
    assert(artifact != NULL && capacity >= total);
    memset(artifact, 0, total);
    memcpy(artifact, "GXRSCAN1", 8U);
    write_u32_le(artifact + 8U, (uint32_t)(sizeof(header_json) - 1U));
    write_u32_le(artifact + 12U, 1U);
    memcpy(artifact + 20U, header_json, sizeof(header_json) - 1U);
    artifact[total - 1U] = marker;
    write_u32_le(artifact + 16U, reference_crc32(artifact + 20U, total - 20U));
    return total;
}


static size_t make_registration_json(unsigned char *buffer, size_t capacity, const char *scan_id)
{
    const int written = snprintf(
        (char *)buffer,
        capacity,
        "{\"schema\":1,\"scan_frame_id\":\"%s\",\"source\":\"apriltag\"}",
        scan_id
    );
    assert(written > 0 && (size_t)written < capacity);
    return (size_t)written;
}


static void prepare_staging_artifacts(unsigned char marker)
{
    const unsigned char scan[] = {marker};
    char scan_id[20];
    make_scan_id(reference_crc32(scan, sizeof(scan)), scan_id);
    unsigned char occupancy[256];
    unsigned char preview[64];
    unsigned char registration[128];
    const size_t occupancy_size = make_occupancy_header(occupancy, sizeof(occupancy), scan_id);
    const size_t preview_size = make_preview_header(preview, sizeof(preview), reference_crc32(scan, sizeof(scan)));
    const size_t registration_size = make_registration_json(registration, sizeof(registration), scan_id);
    assert(save_kind("scan=save&object_id=OBJ_0001&session_id=staging", scan, sizeof(scan)));
    assert(save_kind("scan=save&kind=occupancy&object_id=OBJ_0001&session_id=staging", occupancy, occupancy_size));
    assert(save_kind("scan=save&kind=preview&object_id=OBJ_0001&session_id=staging", preview, preview_size));
    assert(save_kind("scan=save&kind=registration&object_id=OBJ_0001&session_id=staging", registration, registration_size));
}


static bool file_matches_bytes(const char *path, const unsigned char *expected, size_t length)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    unsigned char buffer[64];
    size_t compared = 0U;
    while (compared < length) {
        size_t chunk_size = length - compared;
        if (chunk_size > sizeof(buffer)) chunk_size = sizeof(buffer);
        if (
            fread(buffer, 1U, chunk_size, file) != chunk_size ||
            memcmp(buffer, expected + compared, chunk_size) != 0
        ) {
            (void)fclose(file);
            return false;
        }
        compared += chunk_size;
    }
    const bool content_ok = fgetc(file) == EOF;
    const int close_result = fclose(file);
    return content_ok && close_result == 0;
}

static void test_scan_save_and_chunked_load_round_trip(void)
{
    make_layout();
    s_storage_begin_calls = 0U;
    s_storage_end_calls = 0U;
    s_priority_changes = 0U;

    static const unsigned char crc_vector[] = "123456789";
    assert(reference_crc32(crc_vector, sizeof(crc_vector) - 1U) == UINT32_C(0xcbf43926));
    const unsigned char artifact[] = {0U, 1U, 2U, 255U, 8U, 13U, 21U};
    char expected_id[20];
    make_scan_id(reference_crc32(artifact, sizeof(artifact)), expected_id);
    char response[512] = {0};
    httpd_req_t save_request = make_request(
        "scan=save&object_id=OBJ_0001&session_id=S000001",
        artifact,
        sizeof(artifact),
        response,
        sizeof(response)
    );
    bool handled = false;
    assert(web_scan_store_try_handle(&save_request, &handled) == ESP_OK);
    assert(handled);
    assert(strstr(response, "\"ok\":true") != NULL);
    assert(strstr(response, "\"bytes\":7") != NULL);
    assert(strstr(response, expected_id) != NULL);
    assert(s_storage_begin_calls == s_storage_end_calls);
    assert(s_priority_changes == 2U);

    memset(response, 0, sizeof(response));
    httpd_req_t load_request = make_request(
        "scan=load&object_id=OBJ_0001&session_id=S000001",
        NULL,
        0U,
        response,
        sizeof(response)
    );
    handled = false;
    assert(web_scan_store_try_handle(&load_request, &handled) == ESP_OK);
    assert(handled);
    assert(load_request.content_type != NULL);
    assert(strcmp(load_request.content_type, "application/octet-stream") == 0);
    assert(load_request.scan_bytes_header != NULL);
    assert(strcmp(load_request.scan_bytes_header, "7") == 0);
    assert(load_request.response_length == sizeof(artifact));
    assert(memcmp(response, artifact, sizeof(artifact)) == 0);
    assert(s_storage_begin_calls == s_storage_end_calls);
    assert(s_priority_changes == 4U);

    remove_layout();
}

static void test_scan_rejects_unknown_session_before_file_access(void)
{
    make_layout();
    s_storage_begin_calls = 0U;
    s_storage_end_calls = 0U;
    char response[512] = {0};
    const unsigned char body[] = {1U};
    httpd_req_t request = make_request(
        "scan=save&object_id=OBJ_0001&session_id=S999999",
        body,
        sizeof(body),
        response,
        sizeof(response)
    );
    bool handled = false;
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK);
    assert(handled);
    assert(request.response_status != NULL);
    assert(strcmp(request.response_status, "404 Not Found") == 0);
    assert(strstr(response, "\"ok\":false") != NULL);
    assert(s_storage_begin_calls == 0U);
    assert(s_storage_end_calls == 0U);
    remove_layout();
}

static bool save_kind(const char *query, const unsigned char *body, size_t length)
{
    char response[512] = {0};
    httpd_req_t request = make_request(query, body, length, response, sizeof(response));
    bool handled = false;
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK);
    assert(handled);
    return strstr(response, "\"ok\":true") != NULL;
}

static void write_sparse_file(const char *file_name, size_t size)
{
    char path[256];
    const int written = snprintf(
        path,
        sizeof(path),
        STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/%s",
        file_name
    );
    assert(written > 0 && (size_t)written < sizeof(path));
    assert(size > 0U && size - 1U <= (size_t)LONG_MAX);

    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fseek(file, (long)(size - 1U), SEEK_SET) == 0);
    assert(fputc(0x5a, file) == 0x5a);
    assert(fclose(file) == 0);
}

static size_t load_kind(const char *query, char *response, size_t capacity)
{
    httpd_req_t request = make_request(query, NULL, 0U, response, capacity);
    bool handled = false;
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK);
    assert(handled);
    return request.response_length;
}


static void request_info_kind(const char *kind_name, char *response, size_t capacity, httpd_req_t *request_out)
{
    char query[160];
    const int written = strcmp(kind_name, "scan") == 0
        ? snprintf(query, sizeof(query), "scan=info&object_id=OBJ_0001&session_id=S000001")
        : snprintf(query, sizeof(query), "scan=info&kind=%s&object_id=OBJ_0001&session_id=S000001", kind_name);
    assert(written > 0 && (size_t)written < sizeof(query));
    *request_out = make_request(query, NULL, 0U, response, capacity);
    bool handled = false;
    assert(web_scan_store_try_handle(request_out, &handled) == ESP_OK && handled);
}

static void stage_query(
    char *query,
    size_t query_size,
    const char *action,
    const char *kind
)
{
    const int written = snprintf(
        query,
        query_size,
        "scan=%s&kind=%s&object_id=OBJ_0001&session_id=staging",
        action,
        kind
    );
    assert(written > 0 && (size_t)written < query_size);
}

static void test_staging_round_trip_kinds_and_limits(void)
{
    make_layout();
    static const unsigned char scan_payload[] = {0x11U};
    char scan_id[20];
    make_scan_id(reference_crc32(scan_payload, sizeof(scan_payload)), scan_id);
    unsigned char occupancy[256];
    unsigned char preview[64];
    unsigned char registration[128];
    const size_t occupancy_size = make_occupancy_header(occupancy, sizeof(occupancy), scan_id);
    const size_t preview_size = make_preview_header(preview, sizeof(preview), reference_crc32(scan_payload, sizeof(scan_payload)));
    const size_t registration_size = make_registration_json(registration, sizeof(registration), scan_id);
    assert(save_kind("scan=save&object_id=OBJ_0001&session_id=staging", scan_payload, sizeof(scan_payload)));
    const struct {
        const char *name;
        const web_scan_kind_t *kind;
        const unsigned char *payload;
        size_t payload_size;
    } cases[] = {
        {"occupancy", &WEB_SCAN_KIND_OCCUPANCY, occupancy, occupancy_size},
        {"preview", &WEB_SCAN_KIND_PREVIEW, preview, preview_size},
        {"registration", &WEB_SCAN_KIND_REGISTRATION, registration, registration_size},
    };
    const unsigned session_checks_before = s_session_list_calls;

    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char query[160], response[512] = {0};
        stage_query(query, sizeof(query), "save", cases[i].name);
        assert(save_kind(query, cases[i].payload, cases[i].payload_size));

        char final_path[WEB_SCAN_PATH_SIZE], temp_path[WEB_SCAN_PATH_SIZE], backup_path[WEB_SCAN_PATH_SIZE];
        assert(web_scan_paths("OBJ_0001", "staging", cases[i].kind, final_path, temp_path, backup_path) == ESP_OK);
        assert(path_exists(final_path));

        stage_query(query, sizeof(query), "load", cases[i].name);
        assert(load_kind(query, response, sizeof(response)) == cases[i].payload_size);
        assert(memcmp(response, cases[i].payload, cases[i].payload_size) == 0);

        memset(response, 0, sizeof(response));
        stage_query(query, sizeof(query), "info", cases[i].name);
        httpd_req_t info = make_request(query, NULL, 0U, response, sizeof(response));
        bool handled = false;
        assert(web_scan_store_try_handle(&info, &handled) == ESP_OK && handled);
        assert(strstr(response, "\"ok\":true") != NULL);
        char expected_bytes[32];
        const int bytes_written = snprintf(
            expected_bytes,
            sizeof(expected_bytes),
            "\"bytes\":%u",
            (unsigned)cases[i].payload_size
        );
        assert(bytes_written > 0 && (size_t)bytes_written < sizeof(expected_bytes));
        assert(strstr(response, expected_bytes) != NULL);

        stage_query(query, sizeof(query), "delete", cases[i].name);
        assert(save_kind(query, NULL, 0U));
        assert(!path_exists(final_path));

        memset(response, 0, sizeof(response));
        stage_query(query, sizeof(query), "save", cases[i].name);
        httpd_req_t oversized = make_request(
            query,
            cases[i].payload,
            cases[i].kind->max_bytes + 1U,
            response,
            sizeof(response)
        );
        assert(web_scan_store_try_handle(&oversized, &handled) == ESP_OK && handled);
        assert(oversized.response_status != NULL && strcmp(oversized.response_status, "400 Bad Request") == 0);
        assert(strstr(response, "ESP_ERR_INVALID_SIZE") != NULL);
        assert(oversized.body_offset == 0U);
    }

    assert(s_session_list_calls == session_checks_before);
    remove_layout();
}

static void test_staging_accepts_only_reserved_literal_and_safe_object_ids(void)
{
    make_layout();
    const unsigned char payload = 0x51U;
    char response[512] = {0};
    httpd_req_t request = make_request(
        "scan=save&object_id=OBJ_0001&session_id=stagingX",
        &payload,
        sizeof(payload),
        response,
        sizeof(response)
    );
    bool handled = false;
    const unsigned calls_before_invalid_session = s_session_list_calls;
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
    assert(request.response_status != NULL && strcmp(request.response_status, "404 Not Found") == 0);
    assert(s_session_list_calls == calls_before_invalid_session + 1U);

    memset(response, 0, sizeof(response));
    s_storage_begin_calls = 0U;
    s_storage_end_calls = 0U;
    request = make_request(
        "scan=save&object_id=../OBJ_0001&session_id=staging",
        &payload,
        sizeof(payload),
        response,
        sizeof(response)
    );
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
    assert(request.response_status != NULL && strcmp(request.response_status, "400 Bad Request") == 0);
    assert(s_storage_begin_calls == 0U);

    char final_path[WEB_SCAN_PATH_SIZE], temp_path[WEB_SCAN_PATH_SIZE], backup_path[WEB_SCAN_PATH_SIZE];
    assert(web_scan_paths("../OBJ_0001", "staging", &WEB_SCAN_KIND_SCAN, final_path, temp_path, backup_path) == ESP_ERR_INVALID_ARG);
    remove_layout();
}

static void test_staging_operations_return_409_while_protected(void)
{
    make_layout();
    static const char *const kinds[] = {"scan", "occupancy", "preview", "registration"};
    static const char *const actions[] = {"save", "load", "info", "delete"};
    const unsigned char payload = 0x61U;
    prepare_staging_artifacts(payload);

    assert(storage_bulk_admission_enter_protected() == ESP_OK);
    for (size_t kind_index = 0U; kind_index < sizeof(kinds) / sizeof(kinds[0]); ++kind_index) {
        for (size_t action_index = 0U; action_index < sizeof(actions) / sizeof(actions[0]); ++action_index) {
            char query[160], response[512] = {0};
            stage_query(query, sizeof(query), actions[action_index], kinds[kind_index]);
            const bool saving = strcmp(actions[action_index], "save") == 0;
            httpd_req_t request = make_request(
                query,
                saving ? &payload : NULL,
                saving ? sizeof(payload) : 0U,
                response,
                sizeof(response)
            );
            bool handled = false;
            assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
            assert(request.response_status != NULL && strcmp(request.response_status, "409 Conflict") == 0);
            assert(strstr(response, "\"busy_reason\":\"recording_active\"") != NULL);
        }
    }
    storage_bulk_admission_exit_protected();
    remove_layout();
}

static void test_object_change_hook_clears_all_staged_files(void)
{
    make_layout();
    const unsigned char payload = 0x71U;
    prepare_staging_artifacts(payload);

    char final_path[WEB_SCAN_PATH_SIZE], temp_path[WEB_SCAN_PATH_SIZE], backup_path[WEB_SCAN_PATH_SIZE];
    assert(web_scan_paths("OBJ_0001", "staging", &WEB_SCAN_KIND_SCAN, final_path, temp_path, backup_path) == ESP_OK);
    write_bytes(temp_path, &payload, sizeof(payload));
    write_bytes(backup_path, &payload, sizeof(payload));
    assert(web_scan_store_clear_staging("../OBJ_0001") == ESP_ERR_INVALID_ARG);
    assert(path_exists(final_path));
    assert(web_scan_store_clear_staging("OBJ_0001") == ESP_OK);

    const web_scan_kind_t *const artifact_kinds[] = {
        &WEB_SCAN_KIND_SCAN,
        &WEB_SCAN_KIND_OCCUPANCY,
        &WEB_SCAN_KIND_PREVIEW,
        &WEB_SCAN_KIND_REGISTRATION,
    };
    for (size_t i = 0U; i < sizeof(artifact_kinds) / sizeof(artifact_kinds[0]); ++i) {
        assert(web_scan_paths("OBJ_0001", "staging", artifact_kinds[i], final_path, temp_path, backup_path) == ESP_OK);
        assert(!path_exists(final_path));
        assert(!path_exists(temp_path));
        assert(!path_exists(backup_path));
    }
    assert(web_scan_store_clear_staging("OBJ_0001") == ESP_OK);
    remove_layout();
}

static void test_kind_size_validation_boundaries(void)
{
    const struct {
        const web_scan_kind_t *kind;
        size_t limit;
    } cases[] = {
        {&WEB_SCAN_KIND_SCAN, 8U * 1024U * 1024U},
        {&WEB_SCAN_KIND_OCCUPANCY, 1024U * 1024U},
        {&WEB_SCAN_KIND_PREVIEW, 65536U},
        {&WEB_SCAN_KIND_REGISTRATION, 16384U},
    };

    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        assert(cases[i].kind->max_bytes == cases[i].limit);
        assert(!web_scan_kind_size_valid(cases[i].kind, 0L));
        assert(web_scan_kind_size_valid(cases[i].kind, (long)cases[i].limit));
        assert(!web_scan_kind_size_valid(cases[i].kind, (long)(cases[i].limit + 1U)));
    }
    assert(!web_scan_kind_size_valid(NULL, 1L));
}

static void test_scan_and_occupancy_size_limits(void)
{
    make_layout();
    const size_t scan_limit = WEB_SCAN_KIND_SCAN.max_bytes;
    const size_t occupancy_limit = WEB_SCAN_KIND_OCCUPANCY.max_bytes;
    const size_t medium_scan_size = occupancy_limit + 1U;
    unsigned char *body = malloc(scan_limit + 1U);
    assert(body != NULL);
    memset(body, 0xa5, scan_limit + 1U);

    char response[512] = {0};
    httpd_req_t request = make_request(
        "scan=save&object_id=OBJ_0001&session_id=S000001",
        body,
        scan_limit,
        response,
        sizeof(response)
    );
    bool handled = false;
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
    assert(strstr(response, "\"ok\":true") != NULL);
    assert(strstr(response, "\"bytes\":8388608") != NULL);
    assert(request.body_offset == scan_limit);
    const uint32_t initial_scan_crc = reference_crc32(body, scan_limit);

    memset(response, 0, sizeof(response));
    request = make_request(
        "scan=save&object_id=OBJ_0001&session_id=S000001",
        body,
        scan_limit + 1U,
        response,
        sizeof(response)
    );
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
    assert(request.response_status != NULL && strcmp(request.response_status, "400 Bad Request") == 0);
    assert(strstr(response, "ESP_ERR_INVALID_SIZE") != NULL);
    assert(request.body_offset == 0U);

    const size_t preview_size = make_large_preview(
        body,
        scan_limit + 1U,
        WEB_SCAN_KIND_PREVIEW.max_bytes,
        initial_scan_crc
    );
    memset(response, 0, sizeof(response));
    request = make_request(
        "scan=save&kind=preview&object_id=OBJ_0001&session_id=S000001",
        body,
        preview_size,
        response,
        sizeof(response)
    );
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
    assert(strstr(response, "\"ok\":true") != NULL);
    assert(request.body_offset == preview_size);

    memset(response, 0, sizeof(response));
    request = make_request(
        "scan=save&kind=preview&object_id=OBJ_0001&session_id=S000001",
        body,
        WEB_SCAN_KIND_PREVIEW.max_bytes + 1U,
        response,
        sizeof(response)
    );
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
    assert(request.response_status != NULL && strcmp(request.response_status, "400 Bad Request") == 0);
    assert(strstr(response, "ESP_ERR_INVALID_SIZE") != NULL);
    assert(request.body_offset == 0U);

    /* A scan larger than the occupancy cap remains valid. */
    memset(response, 0, sizeof(response));
    request = make_request(
        "scan=save&object_id=OBJ_0001&session_id=S000001",
        body,
        medium_scan_size,
        response,
        sizeof(response)
    );
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
    assert(strstr(response, "\"ok\":true") != NULL);
    assert(request.body_offset == medium_scan_size);

    char medium_scan_id[20];
    make_scan_id(reference_crc32(body, medium_scan_size), medium_scan_id);

    char *large_response = malloc(medium_scan_size);
    assert(large_response != NULL);
    memset(large_response, 0, medium_scan_size);
    request = make_request(
        "scan=load&object_id=OBJ_0001&session_id=S000001",
        NULL,
        0U,
        large_response,
        medium_scan_size
    );
    s_chunked_response_bytes = 0U;
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
    assert(request.response_length == medium_scan_size);
    assert(s_chunked_response_bytes == medium_scan_size);
    assert(memcmp(large_response, body, medium_scan_size) == 0);

    const size_t occupancy_size = make_large_occupancy(
        body,
        scan_limit + 1U,
        occupancy_limit,
        medium_scan_id
    );

    memset(response, 0, sizeof(response));
    request = make_request(
        "scan=save&kind=occupancy&object_id=OBJ_0001&session_id=S000001",
        body,
        occupancy_size,
        response,
        sizeof(response)
    );
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
    assert(strstr(response, "\"ok\":true") != NULL);
    assert(request.body_offset == occupancy_size);

    memset(response, 0, sizeof(response));
    request = make_request(
        "scan=save&kind=occupancy&object_id=OBJ_0001&session_id=S000001",
        body,
        occupancy_limit + 1U,
        response,
        sizeof(response)
    );
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
    assert(request.response_status != NULL && strcmp(request.response_status, "400 Bad Request") == 0);
    assert(strstr(response, "ESP_ERR_INVALID_SIZE") != NULL);
    assert(request.body_offset == 0U);

    memset(response, 0, sizeof(response));
    request = make_request(
        "scan=info&kind=occupancy&object_id=OBJ_0001&session_id=S000001",
        NULL,
        0U,
        response,
        sizeof(response)
    );
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
    assert(request.response_status == NULL);
    assert(strstr(response, "\"ok\":true") != NULL);
    assert(strstr(response, "\"bytes\":1048576") != NULL);

    memset(large_response, 0, occupancy_limit);
    request = make_request(
        "scan=load&kind=occupancy&object_id=OBJ_0001&session_id=S000001",
        NULL,
        0U,
        large_response,
        occupancy_limit
    );
    s_chunked_response_bytes = 0U;
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
    assert(request.response_length == occupancy_limit);
    assert(s_chunked_response_bytes == occupancy_limit);
    assert(memcmp(large_response, body, occupancy_limit) == 0);

    write_sparse_file("occupancy-v1.bin", occupancy_limit + 1U);
    memset(response, 0, sizeof(response));
    request = make_request(
        "scan=load&kind=occupancy&object_id=OBJ_0001&session_id=S000001",
        NULL,
        0U,
        response,
        sizeof(response)
    );
    s_chunked_response_bytes = 0U;
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
    assert(request.response_status != NULL && strcmp(request.response_status, "400 Bad Request") == 0);
    assert(strstr(response, "ESP_ERR_INVALID_SIZE") != NULL);
    assert(s_chunked_response_bytes == 0U);

    memset(response, 0, sizeof(response));
    request = make_request(
        "scan=info&kind=occupancy&object_id=OBJ_0001&session_id=S000001",
        NULL,
        0U,
        response,
        sizeof(response)
    );
    s_chunked_response_bytes = 0U;
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
    assert(request.response_status != NULL && strcmp(request.response_status, "400 Bad Request") == 0);
    assert(strstr(response, "ESP_ERR_INVALID_SIZE") != NULL);
    assert(s_chunked_response_bytes == 0U);

    free(large_response);
    free(body);
    remove_layout();
}

/* The compact occupancy and preview artifacts live beside the scan, have
 * independent kind routes, and are deleted with the source scan. */
static void test_occupancy_kind_is_stored_beside_the_scan(void)
{
    make_layout();
    const unsigned char scan[] = {1U, 2U, 3U};
    unsigned char occupancy[256];
    unsigned char preview[64];
    char scan_id[20];
    make_scan_id(reference_crc32(scan, sizeof(scan)), scan_id);
    const size_t occupancy_size = make_occupancy_header(occupancy, sizeof(occupancy), scan_id);
    const size_t preview_size = make_preview_header(preview, sizeof(preview), reference_crc32(scan, sizeof(scan)));
    assert(save_kind("scan=save&object_id=OBJ_0001&session_id=S000001", scan, sizeof(scan)));
    assert(save_kind("scan=save&kind=occupancy&object_id=OBJ_0001&session_id=S000001", occupancy, occupancy_size));
    assert(save_kind("scan=save&kind=preview&object_id=OBJ_0001&session_id=S000001", preview, preview_size));

    char response[512] = {0};
    assert(load_kind("scan=load&kind=occupancy&object_id=OBJ_0001&session_id=S000001", response, sizeof(response)) == occupancy_size);
    assert(memcmp(response, occupancy, occupancy_size) == 0);
    memset(response, 0, sizeof(response));
    assert(load_kind("scan=load&kind=preview&object_id=OBJ_0001&session_id=S000001", response, sizeof(response)) == preview_size);
    assert(memcmp(response, preview, preview_size) == 0);
    memset(response, 0, sizeof(response));
    assert(load_kind("scan=load&object_id=OBJ_0001&session_id=S000001", response, sizeof(response)) == sizeof(scan));
    assert(memcmp(response, scan, sizeof(scan)) == 0);

    /* MAX-169: info reports presence and size without streaming. */
    {
        char info[512] = {0};
        httpd_req_t info_request = make_request(
            "scan=info&object_id=OBJ_0001&session_id=S000001", NULL, 0U, info, sizeof(info));
        bool info_handled = false;
        assert(web_scan_store_try_handle(&info_request, &info_handled) == ESP_OK);
        assert(info_handled);
        assert(strstr(info, "\"ok\":true") != NULL);
        assert(strstr(info, "\"action\":\"scan_info\"") != NULL);
        assert(strstr(info, "\"bytes\":3") != NULL);
        assert(strstr(info, scan_id) != NULL);
        s_scan_identity_cache.valid = false;
        memset(info, 0, sizeof(info));
        info_request = make_request(
            "scan=info&object_id=OBJ_0001&session_id=S000001", NULL, 0U, info, sizeof(info));
        assert(web_scan_store_try_handle(&info_request, &info_handled) == ESP_OK);
        assert(strstr(info, scan_id) != NULL);
        memset(info, 0, sizeof(info));
        info_request = make_request(
            "scan=info&kind=occupancy&object_id=OBJ_0001&session_id=S000001", NULL, 0U, info, sizeof(info));
        assert(web_scan_store_try_handle(&info_request, &info_handled) == ESP_OK);
        assert(strstr(info, "\"source_id\":\"scan-crc32:") != NULL);
        memset(info, 0, sizeof(info));
        info_request = make_request(
            "scan=info&kind=preview&object_id=OBJ_0001&session_id=S000001", NULL, 0U, info, sizeof(info));
        assert(web_scan_store_try_handle(&info_request, &info_handled) == ESP_OK);
        assert(strstr(info, "\"bytes\":58") != NULL);
        assert(strstr(info, "\"source_id\":\"scan-crc32:") != NULL);
    }

    const char *occupancy_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/occupancy-v1.bin";
    const char *preview_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/preview-v1.bin";
    const unsigned char garbage[] = {'n', 'o', 't', '-', 'a', '-', 'h', 'e', 'a', 'd', 'e', 'r'};
    write_bytes(occupancy_path, garbage, sizeof(garbage));
    write_bytes(preview_path, garbage, sizeof(garbage));
    {
        char info[512] = {0};
        bool info_handled = false;
        httpd_req_t info_request = make_request(
            "scan=info&kind=occupancy&object_id=OBJ_0001&session_id=S000001", NULL, 0U, info, sizeof(info));
        assert(web_scan_store_try_handle(&info_request, &info_handled) == ESP_OK && info_handled);
        assert(strstr(info, "\"source_id\":null") != NULL);
        memset(info, 0, sizeof(info));
        info_request = make_request(
            "scan=info&kind=preview&object_id=OBJ_0001&session_id=S000001", NULL, 0U, info, sizeof(info));
        assert(web_scan_store_try_handle(&info_request, &info_handled) == ESP_OK && info_handled);
        assert(strstr(info, "\"source_id\":null") != NULL);
    }

    unsigned char registration[128];
    make_scan_id(reference_crc32(scan, sizeof(scan)), scan_id);
    const size_t registration_size = make_registration_json(registration, sizeof(registration), scan_id);
    const char *registration_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/registration-v1.json";
    const char *registration_temp = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/registration-v1.json.tmp";
    const char *registration_backup = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/registration-v1.json.bak";
    assert(save_kind(
        "scan=save&kind=registration&object_id=OBJ_0001&session_id=S000001",
        registration,
        registration_size
    ));
    write_bytes(registration_temp, registration, registration_size);
    write_bytes(registration_backup, registration, registration_size);

    /* Deleting the scan also removes its registration and derivatives. */
    assert(save_kind("scan=delete&object_id=OBJ_0001&session_id=S000001", NULL, 0U));
    assert(!s_scan_identity_cache.valid);
    assert(!path_exists(registration_path));
    assert(!path_exists(registration_temp));
    assert(!path_exists(registration_backup));
    assert(!path_exists(occupancy_path));
    assert(!path_exists(preview_path));
    {
        char info[512] = {0};
        httpd_req_t info_request = make_request(
            "scan=info&object_id=OBJ_0001&session_id=S000001", NULL, 0U, info, sizeof(info));
        bool info_handled = false;
        assert(web_scan_store_try_handle(&info_request, &info_handled) == ESP_OK);
        assert(strstr(info, "ESP_ERR_NOT_FOUND") != NULL);
        assert(strstr(info, "\"id\":null") != NULL);
    }
    memset(response, 0, sizeof(response));
    httpd_req_t request = make_request(
        "scan=load&kind=occupancy&object_id=OBJ_0001&session_id=S000001", NULL, 0U, response, sizeof(response)
    );
    bool handled = false;
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK);
    assert(request.response_status != NULL);
    assert(strcmp(request.response_status, "404 Not Found") == 0);
    memset(response, 0, sizeof(response));
    request = make_request(
        "scan=load&kind=preview&object_id=OBJ_0001&session_id=S000001", NULL, 0U, response, sizeof(response));
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
    assert(request.response_status != NULL && strcmp(request.response_status, "404 Not Found") == 0);
    memset(response, 0, sizeof(response));
    request = make_request(
        "scan=info&kind=preview&object_id=OBJ_0001&session_id=S000001", NULL, 0U, response, sizeof(response));
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
    assert(strstr(response, "ESP_ERR_NOT_FOUND") != NULL);

    /* Unknown kinds are rejected; oversized occupancy is refused. */
    assert(!save_kind("scan=save&kind=bogus&object_id=OBJ_0001&session_id=S000001", scan, sizeof(scan)));
    remove_layout();
}

static void test_scan_replacement_invalidates_derivatives_and_returns_crc(void)
{
    make_layout();
    const unsigned char scan_a[] = {1U, 2U, 3U, 4U};
    const unsigned char scan_b[] = {9U, 8U, 7U, 6U, 5U};
    char scan_a_id[20];
    make_scan_id(reference_crc32(scan_a, sizeof(scan_a)), scan_a_id);
    unsigned char occupancy[256];
    unsigned char preview[64];
    unsigned char registration[128];
    const size_t occupancy_size = make_occupancy_header(occupancy, sizeof(occupancy), scan_a_id);
    const size_t preview_size = make_preview_header(preview, sizeof(preview), reference_crc32(scan_a, sizeof(scan_a)));
    const size_t registration_size = make_registration_json(registration, sizeof(registration), scan_a_id);
    const char *scan_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/scan-v1.bin";
    const char *occupancy_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/occupancy-v1.bin";
    const char *occupancy_temp = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/occupancy-v1.bin.tmp";
    const char *occupancy_backup = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/occupancy-v1.bin.bak";
    const char *preview_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/preview-v1.bin";
    const char *preview_temp = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/preview-v1.bin.tmp";
    const char *preview_backup = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/preview-v1.bin.bak";
    const char *registration_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/registration-v1.json";
    const char *registration_temp = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/registration-v1.json.tmp";
    const char *registration_backup = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/registration-v1.json.bak";

    assert(save_kind("scan=save&object_id=OBJ_0001&session_id=S000001", scan_a, sizeof(scan_a)));
    assert(save_kind("scan=save&kind=occupancy&object_id=OBJ_0001&session_id=S000001", occupancy, occupancy_size));
    assert(save_kind("scan=save&kind=preview&object_id=OBJ_0001&session_id=S000001", preview, preview_size));
    assert(save_kind(
        "scan=save&kind=registration&object_id=OBJ_0001&session_id=S000001",
        registration,
        registration_size
    ));
    write_bytes(occupancy_temp, occupancy, occupancy_size);
    write_bytes(occupancy_backup, occupancy, occupancy_size);
    write_bytes(preview_temp, preview, preview_size);
    write_bytes(preview_backup, preview, preview_size);
    write_bytes(registration_temp, registration, registration_size);
    write_bytes(registration_backup, registration, registration_size);

    char expected_id[20];
    make_scan_id(reference_crc32(scan_b, sizeof(scan_b)), expected_id);
    char response[512] = {0};
    httpd_req_t request = make_request(
        "scan=save&object_id=OBJ_0001&session_id=S000001",
        scan_b,
        sizeof(scan_b),
        response,
        sizeof(response)
    );
    bool handled = false;
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
    assert(strstr(response, expected_id) != NULL);
    assert(file_matches_bytes(scan_path, scan_b, sizeof(scan_b)));
    assert(!path_exists(occupancy_path) && !path_exists(occupancy_temp) && !path_exists(occupancy_backup));
    assert(!path_exists(preview_path) && !path_exists(preview_temp) && !path_exists(preview_backup));
    /* Issue #31: the registration of the replaced scan is retired (kept as .stale for the record), not left active. */
    char registration_stale[WEB_SCAN_PATH_SIZE];
    assert(snprintf(registration_stale, sizeof(registration_stale), "%s.stale", registration_path) < (int)sizeof(registration_stale));
    assert(!path_exists(registration_path) && !path_exists(registration_temp) && !path_exists(registration_backup));
    assert(file_matches_bytes(registration_stale, registration, registration_size));
    (void)remove(registration_stale);
    remove_layout();
}


/* Issue #31: a registration stays active only for the scan it is bound to. */
static void test_registration_is_bound_to_the_scan_it_was_saved_for(void)
{
    make_layout();
    const unsigned char scan_a[] = {1U, 2U, 3U, 4U};
    const unsigned char scan_b[] = {9U, 8U, 7U, 6U, 5U};
    char scan_a_id[20];
    char scan_b_id[20];
    make_scan_id(reference_crc32(scan_a, sizeof(scan_a)), scan_a_id);
    make_scan_id(reference_crc32(scan_b, sizeof(scan_b)), scan_b_id);
    unsigned char registration_a[128];
    unsigned char registration_b[128];
    const size_t size_a = make_registration_json(registration_a, sizeof(registration_a), scan_a_id);
    const size_t size_b = make_registration_json(registration_b, sizeof(registration_b), scan_b_id);
    const char *scan_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/scan-v1.bin";
    const char *registration_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/registration-v1.json";
    const char *registration_stale = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/registration-v1.json.stale";

    assert(save_kind("scan=save&object_id=OBJ_0001&session_id=S000001", scan_a, sizeof(scan_a)));
    assert(save_kind("scan=save&kind=registration&object_id=OBJ_0001&session_id=S000001", registration_a, size_a));
    assert(file_matches_bytes(registration_path, registration_a, size_a));

    /* A byte-identical re-upload of the same scan keeps its registration active. */
    assert(save_kind("scan=save&object_id=OBJ_0001&session_id=S000001", scan_a, sizeof(scan_a)));
    assert(file_matches_bytes(registration_path, registration_a, size_a));
    assert(!path_exists(registration_stale));

    /* A different scan retires it: no active registration (so the solver falls back to the frozen layout) and the old one is kept aside. */
    assert(save_kind("scan=save&object_id=OBJ_0001&session_id=S000001", scan_b, sizeof(scan_b)));
    assert(file_matches_bytes(scan_path, scan_b, sizeof(scan_b)));
    assert(!path_exists(registration_path));
    assert(file_matches_bytes(registration_stale, registration_a, size_a));

    /* The registration of the new scan is saved and active; a later replacement retires that one in turn. */
    assert(save_kind("scan=save&kind=registration&object_id=OBJ_0001&session_id=S000001", registration_b, size_b));
    assert(file_matches_bytes(registration_path, registration_b, size_b));
    assert(save_kind("scan=save&object_id=OBJ_0001&session_id=S000001", scan_a, sizeof(scan_a)));
    assert(!path_exists(registration_path));
    assert(file_matches_bytes(registration_stale, registration_b, size_b));

    /* A leftover .bak of the old registration cannot be recovered into an active file for the new scan. */
    assert(save_kind("scan=save&kind=registration&object_id=OBJ_0001&session_id=S000001", registration_a, size_a));
    write_bytes(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/registration-v1.json.bak", registration_b, size_b);
    assert(save_kind("scan=save&object_id=OBJ_0001&session_id=S000001", scan_b, sizeof(scan_b)));
    assert(!path_exists(registration_path));
    assert(!path_exists(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/registration-v1.json.bak"));
    char response[512] = {0};
    httpd_req_t info;
    request_info_kind("registration", response, sizeof(response), &info);
    assert(strstr(response, "\"ok\":true") == NULL);                /* nothing is reported as the current registration */

    (void)remove(registration_stale);
    remove_layout();
}


/* Issue #24: deleting the scan of ONE recording unlinks that session's scan-ref; the shared scan and the other sessions stay intact. */
static void test_deleting_a_shared_scan_unlinks_only_this_sessions_reference(void)
{
    make_layout();
    const char *scans = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/scans";
    const char *shared = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/scans/SC000001";
    const char *shared_scan = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/scans/SC000001/scan-v1.bin";
    const char *ref_one = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/scan-ref";
    const char *ref_two = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000002/scan-ref";
    make_directory(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000002");
    make_directory(scans);
    make_directory(shared);
    static const unsigned char scan[] = {7U, 14U, 21U};
    static const unsigned char reference[] = "SC000001";
    write_bytes(shared_scan, scan, sizeof(scan));
    write_bytes(ref_one, reference, sizeof(reference) - 1U);
    write_bytes(ref_two, reference, sizeof(reference) - 1U);

    assert(save_kind("scan=delete&object_id=OBJ_0001&session_id=S000001", NULL, 0U));
    assert(!path_exists(ref_one));                                   /* this recording no longer resolves the shared scan */
    assert(path_exists(ref_two));                                    /* the other recording keeps its reference... */
    assert(file_matches_bytes(shared_scan, scan, sizeof(scan)));     /* ...and the shared scan itself is untouched */

    /* Deleting again is still a success (nothing left to unlink) and changes nothing else. */
    assert(save_kind("scan=delete&object_id=OBJ_0001&session_id=S000001", NULL, 0U));
    assert(path_exists(ref_two) && file_matches_bytes(shared_scan, scan, sizeof(scan)));

    (void)remove(ref_two);
    (void)remove(shared_scan);
    (void)_rmdir(shared);
    (void)_rmdir(scans);
    (void)_rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000002");
    remove_layout();
}


static void test_scan_replacement_delete_failure_keeps_old_scan(void)
{
    make_layout();
    const unsigned char scan_a[] = {1U, 3U, 5U};
    const unsigned char scan_b[] = {2U, 4U, 6U};
    char scan_a_id[20];
    make_scan_id(reference_crc32(scan_a, sizeof(scan_a)), scan_a_id);
    unsigned char occupancy[256];
    unsigned char preview[64];
    const size_t occupancy_size = make_occupancy_header(occupancy, sizeof(occupancy), scan_a_id);
    const size_t preview_size = make_preview_header(preview, sizeof(preview), reference_crc32(scan_a, sizeof(scan_a)));
    const char *scan_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/scan-v1.bin";
    const char *scan_temp = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/scan-v1.bin.tmp";
    const char *occupancy_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/occupancy-v1.bin";
    const char *preview_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/preview-v1.bin";
    assert(save_kind("scan=save&object_id=OBJ_0001&session_id=S000001", scan_a, sizeof(scan_a)));
    assert(save_kind("scan=save&kind=occupancy&object_id=OBJ_0001&session_id=S000001", occupancy, occupancy_size));
    assert(save_kind("scan=save&kind=preview&object_id=OBJ_0001&session_id=S000001", preview, preview_size));

    s_fail_remove_path = occupancy_path;
    char response[512] = {0};
    httpd_req_t request = make_request(
        "scan=save&object_id=OBJ_0001&session_id=S000001",
        scan_b,
        sizeof(scan_b),
        response,
        sizeof(response)
    );
    bool handled = false;
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
    assert(request.response_status != NULL && strcmp(request.response_status, "500 Internal Server Error") == 0);
    assert(file_matches_bytes(scan_path, scan_a, sizeof(scan_a)));
    assert(!path_exists(scan_temp));
    assert(file_matches_bytes(occupancy_path, occupancy, occupancy_size));
    assert(file_matches_bytes(preview_path, preview, preview_size));
    remove_layout();
}


static void test_scan_replacement_power_loss_ordering(void)
{
    const unsigned char scan_a[] = {1U, 3U, 5U};
    const unsigned char scan_b[] = {2U, 4U, 6U};
    char scan_a_id[20];
    make_scan_id(reference_crc32(scan_a, sizeof(scan_a)), scan_a_id);
    unsigned char occupancy[256];
    unsigned char preview[64];
    const size_t occupancy_size = make_occupancy_header(occupancy, sizeof(occupancy), scan_a_id);
    const size_t preview_size = make_preview_header(preview, sizeof(preview), reference_crc32(scan_a, sizeof(scan_a)));
    const char *scan_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/scan-v1.bin";
    const char *occupancy_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/occupancy-v1.bin";
    const char *preview_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/preview-v1.bin";
    const char *preview_backup = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/preview-v1.bin.bak";

    make_layout();
    assert(save_kind("scan=save&object_id=OBJ_0001&session_id=S000001", scan_a, sizeof(scan_a)));
    assert(save_kind("scan=save&kind=occupancy&object_id=OBJ_0001&session_id=S000001", occupancy, occupancy_size));
    assert(save_kind("scan=save&kind=preview&object_id=OBJ_0001&session_id=S000001", preview, preview_size));
    s_cut_after_remove_path = preview_backup;
    char response[512] = {0};
    httpd_req_t request = make_request(
        "scan=save&object_id=OBJ_0001&session_id=S000001",
        scan_b,
        sizeof(scan_b),
        response,
        sizeof(response)
    );
    bool handled = false;
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
    assert(file_matches_bytes(scan_path, scan_a, sizeof(scan_a)));
    assert(!path_exists(occupancy_path) && !path_exists(preview_path));
    assert(!s_scan_identity_cache.valid);
    remove_layout();

    make_layout();
    assert(save_kind("scan=save&object_id=OBJ_0001&session_id=S000001", scan_a, sizeof(scan_a)));
    assert(save_kind("scan=save&kind=occupancy&object_id=OBJ_0001&session_id=S000001", occupancy, occupancy_size));
    assert(save_kind("scan=save&kind=preview&object_id=OBJ_0001&session_id=S000001", preview, preview_size));
    s_cut_after_rename_path = scan_path;
    memset(response, 0, sizeof(response));
    request = make_request(
        "scan=save&object_id=OBJ_0001&session_id=S000001",
        scan_b,
        sizeof(scan_b),
        response,
        sizeof(response)
    );
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
    assert(file_matches_bytes(scan_path, scan_b, sizeof(scan_b)));
    assert(!path_exists(occupancy_path) && !path_exists(preview_path));
    remove_layout();
}


static void test_derivative_compare_and_commit_rejects_stale_uploads(void)
{
    make_layout();
    static const unsigned char scan_a[] = {10U, 20U, 30U, 40U};
    static const unsigned char scan_b[] = {1U, 3U, 5U, 7U};
    char scan_a_id[20];
    char scan_b_id[20];
    make_scan_id(reference_crc32(scan_a, sizeof(scan_a)), scan_a_id);
    make_scan_id(reference_crc32(scan_b, sizeof(scan_b)), scan_b_id);
    unsigned char occupancy_a[256], occupancy_b[256];
    unsigned char preview_a[64], preview_b[64];
    unsigned char registration_a[128], registration_b[128];
    const size_t occupancy_a_size = make_occupancy_header(occupancy_a, sizeof(occupancy_a), scan_a_id);
    const size_t occupancy_b_size = make_occupancy_header(occupancy_b, sizeof(occupancy_b), scan_b_id);
    const size_t preview_a_size = make_preview_header(preview_a, sizeof(preview_a), reference_crc32(scan_a, sizeof(scan_a)));
    const size_t preview_b_size = make_preview_header(preview_b, sizeof(preview_b), reference_crc32(scan_b, sizeof(scan_b)));
    const size_t registration_a_size = make_registration_json(registration_a, sizeof(registration_a), scan_a_id);
    const size_t registration_b_size = make_registration_json(registration_b, sizeof(registration_b), scan_b_id);
    const char *occupancy_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/occupancy-v1.bin";
    const char *preview_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/preview-v1.bin";
    const char *registration_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/registration-v1.json";
    char query[192];
    char response[512] = {0};
    bool handled = false;

    int written = snprintf(
        query,
        sizeof(query),
        "scan=save&kind=occupancy&scan_id=%s&object_id=OBJ_0001&session_id=S000001",
        scan_a_id
    );
    assert(written > 0 && (size_t)written < sizeof(query));
    httpd_req_t request = make_request(query, occupancy_a, occupancy_a_size, response, sizeof(response));
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
    assert(request.response_status != NULL && strcmp(request.response_status, "409 Conflict") == 0);
    assert(strcmp(response, "{\"ok\":false,\"error\":\"scan_generation_mismatch\",\"current_id\":null}") == 0);
    assert(!path_exists(occupancy_path));

    assert(save_kind("scan=save&object_id=OBJ_0001&session_id=S000001", scan_a, sizeof(scan_a)));
    assert(save_kind("scan=save&kind=occupancy&object_id=OBJ_0001&session_id=S000001", occupancy_a, occupancy_a_size));
    assert(save_kind("scan=save&kind=preview&object_id=OBJ_0001&session_id=S000001", preview_a, preview_a_size));
    assert(save_kind("scan=save&kind=registration&object_id=OBJ_0001&session_id=S000001", registration_a, registration_a_size));
    assert(save_kind("scan=save&object_id=OBJ_0001&session_id=S000001", scan_b, sizeof(scan_b)));
    assert(!path_exists(occupancy_path) && !path_exists(preview_path));

    written = snprintf(
        query,
        sizeof(query),
        "scan=save&kind=occupancy&scan_id=%s&object_id=OBJ_0001&session_id=S000001",
        scan_b_id
    );
    assert(written > 0 && (size_t)written < sizeof(query));
    assert(save_kind(query, occupancy_b, occupancy_b_size));
    written = snprintf(
        query,
        sizeof(query),
        "scan=save&kind=preview&scan_id=%s&object_id=OBJ_0001&session_id=S000001",
        scan_b_id
    );
    assert(written > 0 && (size_t)written < sizeof(query));
    assert(save_kind(query, preview_b, preview_b_size));
    written = snprintf(
        query,
        sizeof(query),
        "scan=save&kind=registration&scan_id=%s&object_id=OBJ_0001&session_id=S000001",
        scan_b_id
    );
    assert(written > 0 && (size_t)written < sizeof(query));
    assert(save_kind(query, registration_b, registration_b_size));

    const struct {
        const char *kind;
        const unsigned char *body;
        size_t body_size;
        const char *path;
        const unsigned char *expected;
        size_t expected_size;
    } stale_uploads[] = {
        {"occupancy", occupancy_a, occupancy_a_size, occupancy_path, occupancy_b, occupancy_b_size},
        {"preview", preview_a, preview_a_size, preview_path, preview_b, preview_b_size},
        {"registration", registration_a, registration_a_size, registration_path, registration_b, registration_b_size},
    };
    for (size_t i = 0U; i < sizeof(stale_uploads) / sizeof(stale_uploads[0]); ++i) {
        for (size_t with_query = 0U; with_query < 2U; ++with_query) {
            written = with_query == 0U
                ? snprintf(query, sizeof(query), "scan=save&kind=%s&object_id=OBJ_0001&session_id=S000001", stale_uploads[i].kind)
                : snprintf(query, sizeof(query), "scan=save&kind=%s&scan_id=%s&object_id=OBJ_0001&session_id=S000001", stale_uploads[i].kind, scan_a_id);
            assert(written > 0 && (size_t)written < sizeof(query));
            memset(response, 0, sizeof(response));
            request = make_request(query, stale_uploads[i].body, stale_uploads[i].body_size, response, sizeof(response));
            assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
            assert(request.response_status != NULL && strcmp(request.response_status, "409 Conflict") == 0);
            assert(strstr(response, "\"error\":\"scan_generation_mismatch\"") != NULL);
            assert(strstr(response, scan_b_id) != NULL);
            assert(file_matches_bytes(stale_uploads[i].path, stale_uploads[i].expected, stale_uploads[i].expected_size));
        }
    }

    /* A query bound to B cannot relabel content bound to A. */
    written = snprintf(
        query,
        sizeof(query),
        "scan=save&kind=occupancy&scan_id=%s&object_id=OBJ_0001&session_id=S000001",
        scan_b_id
    );
    assert(written > 0 && (size_t)written < sizeof(query));
    memset(response, 0, sizeof(response));
    request = make_request(query, occupancy_a, occupancy_a_size, response, sizeof(response));
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
    assert(request.response_status != NULL && strcmp(request.response_status, "409 Conflict") == 0);
    assert(file_matches_bytes(occupancy_path, occupancy_b, occupancy_b_size));
    remove_layout();
}


static void test_derivative_compare_and_commit_toctou_recheck(void)
{
    make_layout();
    static const unsigned char scan_a[] = {2U, 4U, 6U, 8U};
    static const unsigned char scan_b[] = {0x71U, 0x72U, 0x73U, 0x74U};
    char scan_a_id[20];
    char scan_b_id[20];
    make_scan_id(reference_crc32(scan_a, sizeof(scan_a)), scan_a_id);
    make_scan_id(reference_crc32(scan_b, sizeof(scan_b)), scan_b_id);
    unsigned char occupancy[256];
    const size_t occupancy_size = make_occupancy_header(occupancy, sizeof(occupancy), scan_a_id);
    const char *occupancy_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/occupancy-v1.bin";
    const char *occupancy_temp = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/occupancy-v1.bin.tmp";
    char response[512] = {0};
    httpd_req_t request = make_request(
        "scan=save&object_id=OBJ_0001&session_id=S000001",
        scan_a,
        sizeof(scan_a),
        response,
        sizeof(response)
    );
    bool handled = false;
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);

    s_replace_scan_before_derivative_commit = true;
    memset(response, 0, sizeof(response));
    request = make_request(
        "scan=save&kind=occupancy&object_id=OBJ_0001&session_id=S000001",
        occupancy,
        occupancy_size,
        response,
        sizeof(response)
    );
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
    assert(request.response_status != NULL && strcmp(request.response_status, "409 Conflict") == 0);
    assert(strstr(response, "\"error\":\"scan_generation_mismatch\"") != NULL);
    assert(strstr(response, scan_b_id) != NULL);
    assert(!path_exists(occupancy_path) && !path_exists(occupancy_temp));
    remove_layout();
}


/* Issue #30: a scan that disappears between hashing and the identity-cache store is reported as missing, never as present with a stale id. */
static void test_identity_cache_store_reports_a_scan_that_vanished(void)
{
    make_layout();
    static const unsigned char scan[] = {2U, 4U, 6U, 8U};
    char scan_id[20];
    make_scan_id(reference_crc32(scan, sizeof(scan)), scan_id);
    const char *scan_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/scan-v1.bin";
    assert(save_kind("scan=save&object_id=OBJ_0001&session_id=S000001", scan, sizeof(scan)));
    web_scan_identity_cache_clear_locked(scan_path);
    s_remove_scan_before_identity_cache_store = true;

    char response[512] = {0};
    httpd_req_t info;
    request_info_kind("scan", response, sizeof(response), &info);
    assert(strstr(response, "\"ok\":true") == NULL);
    assert(strstr(response, scan_id) == NULL);                        /* no stale identity claim */
    assert(!path_exists(scan_path));

    /* The race did not poison the cache: asking again just says "not found". */
    char again[512] = {0};
    httpd_req_t second;
    request_info_kind("scan", again, sizeof(again), &second);
    assert(strstr(again, "\"ok\":true") == NULL && strstr(again, scan_id) == NULL);
    remove_layout();
}


static void test_identity_cache_publish_preserves_replacement(void)
{
    make_layout();
    static const unsigned char scan_a[] = {1U, 3U, 5U, 7U};
    static const unsigned char scan_b[] = {0x71U, 0x72U, 0x73U, 0x74U};
    char scan_a_id[20];
    char scan_b_id[20];
    make_scan_id(reference_crc32(scan_a, sizeof(scan_a)), scan_a_id);
    make_scan_id(reference_crc32(scan_b, sizeof(scan_b)), scan_b_id);
    unsigned char occupancy[256];
    const size_t occupancy_size = make_occupancy_header(
        occupancy,
        sizeof(occupancy),
        scan_a_id
    );
    const char *scan_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/scan-v1.bin";
    const char *occupancy_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/occupancy-v1.bin";
    const char *occupancy_temp = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/occupancy-v1.bin.tmp";
    char response[512] = {0};
    assert(save_kind("scan=save&object_id=OBJ_0001&session_id=S000001", scan_a, sizeof(scan_a)));
    web_scan_identity_cache_clear_locked(scan_path);
    s_replace_scan_before_identity_cache_store = true;

    httpd_req_t request = make_request(
        "scan=save&kind=occupancy&object_id=OBJ_0001&session_id=S000001",
        occupancy,
        occupancy_size,
        response,
        sizeof(response)
    );
    bool handled = false;
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
    assert(request.response_status != NULL && strcmp(request.response_status, "409 Conflict") == 0);
    assert(strstr(response, "\"error\":\"scan_generation_mismatch\"") != NULL);
    assert(strstr(response, scan_b_id) != NULL);
    assert(file_matches_bytes(scan_path, scan_b, sizeof(scan_b)));
    assert(!path_exists(occupancy_path) && !path_exists(occupancy_temp));
    remove_layout();
}


static void test_registration_unbound_is_rejected(void)
{
    make_layout();
    static const unsigned char scan[] = {3U, 6U, 9U};
    static const unsigned char registration[] = "{\"schema\":1,\"source\":\"apriltag\"}";
    const char *registration_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/registration-v1.json";
    char response[512] = {0};
    assert(save_kind("scan=save&object_id=OBJ_0001&session_id=S000001", scan, sizeof(scan)));
    httpd_req_t request = make_request(
        "scan=save&kind=registration&object_id=OBJ_0001&session_id=S000001",
        registration,
        sizeof(registration) - 1U,
        response,
        sizeof(response)
    );
    bool handled = false;
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
    assert(request.response_status != NULL && strcmp(request.response_status, "409 Conflict") == 0);
    assert(strcmp(response, "{\"ok\":false,\"error\":\"registration_unbound\"}") == 0);
    assert(!path_exists(registration_path));
    remove_layout();
}


/* Issue #28: the registration admission parses the ROOT object; nested decoys, repeated root keys and a missing root schema are refused. */
static void test_registration_admission_parses_the_root_object(void)
{
    make_layout();
    static const unsigned char scan[] = {5U, 10U, 15U};
    char good[20];
    make_scan_id(reference_crc32(scan, sizeof(scan)), good);
    const char *bad = "scan-crc32:deadbeef";
    const char *registration_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/registration-v1.json";
    assert(save_kind("scan=save&object_id=OBJ_0001&session_id=S000001", scan, sizeof(scan)));

    char body[256];
    const char *templates[] = {
        "{\"schema\":1,\"nested\":{\"scan_frame_id\":\"%s\"},\"scan_frame_id\":\"%s\"}",
        "{\"schema\":1,\"scan_frame_id\":\"%s\",\"scan_frame_id\":\"%s\"}",
        "{\"nested\":{\"schema\":1},\"scan_frame_id\":\"%s\",\"x\":\"%s\"}",
        "{\"schema\":2,\"scan_frame_id\":\"%s\",\"x\":\"%s\"}",
        "{\"schema\":1,\"scan_frame_id\":\"%s\",\"x\":\"%s\"",
        "[{\"schema\":1,\"scan_frame_id\":\"%s\",\"x\":\"%s\"}]",
    };
    for (size_t index = 0U; index < sizeof(templates) / sizeof(templates[0]); ++index) {
        const int written = snprintf(body, sizeof(body), templates[index], good, bad);
        assert(written > 0 && (size_t)written < sizeof(body));
        assert(!save_kind("scan=save&kind=registration&object_id=OBJ_0001&session_id=S000001", (const unsigned char *)body, (size_t)written));
        assert(!path_exists(registration_path));
    }
    /* A root-level binding to the real scan is still accepted. */
    const int written = snprintf(body, sizeof(body), "{\"schema\":1,\"scan_frame_id\":\"%s\",\"source\":\"apriltag\"}", good);
    assert(written > 0 && (size_t)written < sizeof(body));
    assert(save_kind("scan=save&kind=registration&object_id=OBJ_0001&session_id=S000001", (const unsigned char *)body, (size_t)written));
    assert(path_exists(registration_path));
    remove_layout();
}


static void test_registration_round_trip_bounds_and_session_validation(void)
{
    make_layout();
    static const unsigned char scan[] = {4U, 8U, 12U};
    unsigned char registration[128];
    char scan_id[20];
    make_scan_id(reference_crc32(scan, sizeof(scan)), scan_id);
    const size_t registration_size = make_registration_json(registration, sizeof(registration), scan_id);
    const char *registration_kind = "kind=registration&";
    char query[160];
    char response[512] = {0};
    assert(save_kind("scan=save&object_id=OBJ_0001&session_id=S000001", scan, sizeof(scan)));
    (void)snprintf(query, sizeof(query), "scan=save&%sobject_id=OBJ_0001&session_id=S000001", registration_kind);
    assert(save_kind(query, registration, registration_size));

    memset(response, 0, sizeof(response));
    httpd_req_t load = make_request(
        "scan=load&kind=registration&object_id=OBJ_0001&session_id=S000001", NULL, 0U, response, sizeof(response));
    bool handled = false;
    assert(web_scan_store_try_handle(&load, &handled) == ESP_OK && handled);
    assert(load.content_type != NULL && strcmp(load.content_type, "application/json; charset=utf-8") == 0);
    assert(load.response_length == registration_size);
    assert(memcmp(response, registration, registration_size) == 0);

    memset(response, 0, sizeof(response));
    httpd_req_t info = make_request(
        "scan=info&kind=registration&object_id=OBJ_0001&session_id=S000001", NULL, 0U, response, sizeof(response));
    assert(web_scan_store_try_handle(&info, &handled) == ESP_OK && handled);
    assert(strstr(response, "\"ok\":true") != NULL);
    char expected_bytes[32];
    const int bytes_written = snprintf(expected_bytes, sizeof(expected_bytes), "\"bytes\":%u", (unsigned)registration_size);
    assert(bytes_written > 0 && (size_t)bytes_written < sizeof(expected_bytes));
    assert(strstr(response, expected_bytes) != NULL);

    static const unsigned char oversized[16385] = {0U};
    memset(response, 0, sizeof(response));
    httpd_req_t too_large = make_request(
        "scan=save&kind=registration&object_id=OBJ_0001&session_id=S000001", oversized, sizeof(oversized), response, sizeof(response));
    assert(web_scan_store_try_handle(&too_large, &handled) == ESP_OK && handled);
    assert(too_large.response_status != NULL && strcmp(too_large.response_status, "400 Bad Request") == 0);
    assert(strstr(response, "ESP_ERR_INVALID_SIZE") != NULL);

    const char *registration_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/registration-v1.json";
    FILE *oversized_file = fopen(registration_path, "wb");
    assert(oversized_file != NULL);
    assert(fwrite(oversized, 1U, sizeof(oversized), oversized_file) == sizeof(oversized));
    assert(fclose(oversized_file) == 0);
    memset(response, 0, sizeof(response));
    httpd_req_t oversized_load = make_request(
        "scan=load&kind=registration&object_id=OBJ_0001&session_id=S000001", NULL, 0U, response, sizeof(response));
    assert(web_scan_store_try_handle(&oversized_load, &handled) == ESP_OK && handled);
    assert(oversized_load.response_status != NULL && strcmp(oversized_load.response_status, "400 Bad Request") == 0);
    memset(response, 0, sizeof(response));
    httpd_req_t oversized_info = make_request(
        "scan=info&kind=registration&object_id=OBJ_0001&session_id=S000001", NULL, 0U, response, sizeof(response));
    assert(web_scan_store_try_handle(&oversized_info, &handled) == ESP_OK && handled);
    assert(oversized_info.response_status != NULL && strcmp(oversized_info.response_status, "400 Bad Request") == 0);
    assert(remove(registration_path) == 0);

    memset(response, 0, sizeof(response));
    httpd_req_t missing = make_request(
        "scan=save&kind=registration&object_id=OBJ_0001&session_id=S000002", registration, registration_size, response, sizeof(response));
    assert(web_scan_store_try_handle(&missing, &handled) == ESP_OK && handled);
    assert(missing.response_status != NULL && strcmp(missing.response_status, "404 Not Found") == 0);

    memset(response, 0, sizeof(response));
    httpd_req_t delete = make_request(
        "scan=delete&kind=registration&object_id=OBJ_0001&session_id=S000001", NULL, 0U, response, sizeof(response));
    assert(web_scan_store_try_handle(&delete, &handled) == ESP_OK && handled);
    assert(strstr(response, "\"ok\":true") != NULL);
    memset(response, 0, sizeof(response));
    info = make_request("scan=info&kind=registration&object_id=OBJ_0001&session_id=S000001", NULL, 0U, response, sizeof(response));
    assert(web_scan_store_try_handle(&info, &handled) == ESP_OK && handled);
    assert(strstr(response, "ESP_ERR_NOT_FOUND") != NULL);
    remove_layout();
}

static void test_registration_recording_guard_is_session_scoped(void)
{
    make_layout();
    static const unsigned char scan[] = {5U, 10U, 15U};
    unsigned char registration[128];
    char scan_id[20];
    make_scan_id(reference_crc32(scan, sizeof(scan)), scan_id);
    const size_t registration_size = make_registration_json(registration, sizeof(registration), scan_id);
    char response[512] = {0};
    const char *query = "scan=save&kind=registration&object_id=OBJ_0001&session_id=S000001";
    assert(save_kind("scan=save&object_id=OBJ_0001&session_id=S000001", scan, sizeof(scan)));
    assert(save_kind(query, registration, registration_size));

    assert(storage_bulk_admission_enter_protected() == ESP_OK);

    s_recorder_snapshot.running = true;
    (void)snprintf(s_recorder_snapshot.object_id, sizeof(s_recorder_snapshot.object_id), "OBJ_0001");
    (void)snprintf(s_recorder_snapshot.session_id, sizeof(s_recorder_snapshot.session_id), "S000001");
    httpd_req_t save = make_request(query, registration, registration_size, response, sizeof(response));
    bool handled = false;
    assert(web_scan_store_try_handle(&save, &handled) == ESP_OK && handled);
    assert(save.response_status != NULL && strcmp(save.response_status, "409 Conflict") == 0);
    assert(strstr(response, "this session is recording") != NULL);
    assert(strstr(response, "busy_reason") == NULL);

    static const unsigned char oversized[16385] = {0U};
    memset(response, 0, sizeof(response));
    httpd_req_t oversized_save = make_request(query, oversized, sizeof(oversized), response, sizeof(response));
    assert(web_scan_store_try_handle(&oversized_save, &handled) == ESP_OK && handled);
    assert(oversized_save.response_status != NULL && strcmp(oversized_save.response_status, "400 Bad Request") == 0);

    memset(response, 0, sizeof(response));
    httpd_req_t remove = make_request(
        "scan=delete&kind=registration&object_id=OBJ_0001&session_id=S000001", NULL, 0U, response, sizeof(response));
    assert(web_scan_store_try_handle(&remove, &handled) == ESP_OK && handled);
    assert(remove.response_status != NULL && strcmp(remove.response_status, "409 Conflict") == 0);

    memset(response, 0, sizeof(response));
    httpd_req_t load = make_request(
        "scan=load&kind=registration&object_id=OBJ_0001&session_id=S000001", NULL, 0U, response, sizeof(response));
    assert(web_scan_store_try_handle(&load, &handled) == ESP_OK && handled);
    assert(load.response_status == NULL);
    assert(load.response_length == registration_size);

    memset(response, 0, sizeof(response));
    httpd_req_t info = make_request(
        "scan=info&kind=registration&object_id=OBJ_0001&session_id=S000001", NULL, 0U, response, sizeof(response));
    assert(web_scan_store_try_handle(&info, &handled) == ESP_OK && handled);
    assert(strstr(response, "\"ok\":true") != NULL);

    (void)snprintf(s_recorder_snapshot.session_id, sizeof(s_recorder_snapshot.session_id), "S000002");
    memset(response, 0, sizeof(response));
    save = make_request(query, registration, registration_size, response, sizeof(response));
    assert(web_scan_store_try_handle(&save, &handled) == ESP_OK && handled);
    assert(save.response_status == NULL && strstr(response, "\"ok\":true") != NULL);
    s_recorder_snapshot.running = false;
    storage_bulk_admission_exit_protected();
    remove_layout();
}


static void assert_recording_busy(
    const char *query,
    const unsigned char *body,
    size_t body_length,
    const char *action
)
{
    char response[512] = {0};
    httpd_req_t request = make_request(query, body, body_length, response, sizeof(response));
    bool handled = false;
    const unsigned before_session_checks = s_session_list_calls;

    s_storage_begin_calls = 0U;
    s_storage_end_calls = 0U;
    s_recv_calls = 0U;
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK);
    assert(handled);
    assert(request.response_status != NULL);
    assert(strcmp(request.response_status, "409 Conflict") == 0);
    char expected[128];
    const int written = snprintf(
        expected,
        sizeof(expected),
        "{\"ok\":false,\"action\":\"%s\",\"busy_reason\":\"recording_active\"}",
        action
    );
    assert(written > 0 && (size_t)written < sizeof(expected));
    assert(strcmp(response, expected) == 0);
    assert(request.body_offset == body_length);
    assert(s_session_list_calls == before_session_checks);
    assert(s_storage_begin_calls == 0U);
    assert(s_storage_end_calls == 0U);
}


/* ------------------------------------------------------------------------------ kind=tags (tag-positions-v1.json) */

static size_t make_tags_json(unsigned char *buffer, size_t capacity, const char *scan_id_json, const char *nodes)
{
    const int written = snprintf(
        (char *)buffer, capacity,
        "{\"schema\":1,\"kind\":\"tag_positions\",\"frame\":\"sensor_main_origin_z_up_mm\",\"scan_frame_id\":%s,\"created_at_ms\":1700000000000,"
        "\"main_scan_position_mm\":[1000,2000,1000],\"weakest_main_triangle_height_mm\":250,\"nodes\":[%s]}",
        scan_id_json,
        nodes != NULL ? nodes :
        "{\"node_id\":0,\"tag_id\":0,\"x_mm\":0,\"y_mm\":0,\"z_mm\":0,\"uncertainty_mm\":0,\"confirmed\":true,\"observations\":9,\"views\":4},"
        "{\"node_id\":1,\"tag_id\":1,\"x_mm\":500,\"y_mm\":0,\"z_mm\":0,\"uncertainty_mm\":5,\"confirmed\":true,\"observations\":8,\"views\":4},"
        "{\"node_id\":2,\"tag_id\":2,\"x_mm\":0,\"y_mm\":800,\"z_mm\":0,\"uncertainty_mm\":6,\"confirmed\":false,\"observations\":3,\"views\":2}");
    assert(written > 0 && (size_t)written < capacity);
    return (size_t)written;
}

static size_t make_tags_json_for(unsigned char *buffer, size_t capacity, const char *scan_id)
{
    char quoted[40];
    assert(snprintf(quoted, sizeof(quoted), "\"%s\"", scan_id) > 0);
    return make_tags_json(buffer, capacity, quoted, NULL);
}

static void test_tags_round_trip_is_json_bound_and_removed_with_its_scan(void)
{
    make_layout();
    static const unsigned char scan[] = {21U, 22U, 23U};
    unsigned char tags[1024];
    char scan_id[20];
    make_scan_id(reference_crc32(scan, sizeof(scan)), scan_id);
    const size_t tags_size = make_tags_json_for(tags, sizeof(tags), scan_id);
    const char *tags_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/tag-positions-v1.json";
    const char *tags_temp = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/tag-positions-v1.json.tmp";
    const char *tags_backup = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/tag-positions-v1.json.bak";
    char response[1200] = {0};
    bool handled = false;

    assert(save_kind("scan=save&object_id=OBJ_0001&session_id=S000001", scan, sizeof(scan)));
    assert(save_kind("scan=save&kind=tags&object_id=OBJ_0001&session_id=S000001", tags, tags_size));
    assert(path_exists(tags_path));

    httpd_req_t load = make_request("scan=load&kind=tags&object_id=OBJ_0001&session_id=S000001", NULL, 0U, response, sizeof(response));
    assert(web_scan_store_try_handle(&load, &handled) == ESP_OK && handled);
    assert(load.content_type != NULL && strcmp(load.content_type, "application/json; charset=utf-8") == 0);
    assert(load.response_length == tags_size && memcmp(response, tags, tags_size) == 0);

    memset(response, 0, sizeof(response));
    httpd_req_t info = make_request("scan=info&kind=tags&object_id=OBJ_0001&session_id=S000001", NULL, 0U, response, sizeof(response));
    assert(web_scan_store_try_handle(&info, &handled) == ESP_OK && handled);
    char expected_bytes[32];
    assert(snprintf(expected_bytes, sizeof(expected_bytes), "\"bytes\":%u", (unsigned)tags_size) > 0);
    assert(strstr(response, "\"ok\":true") != NULL && strstr(response, expected_bytes) != NULL);

    /* A tags delete removes just that file. */
    memset(response, 0, sizeof(response));
    httpd_req_t remove_tags = make_request("scan=delete&kind=tags&object_id=OBJ_0001&session_id=S000001", NULL, 0U, response, sizeof(response));
    assert(web_scan_store_try_handle(&remove_tags, &handled) == ESP_OK && handled);
    assert(strstr(response, "\"ok\":true") != NULL && !path_exists(tags_path));
    memset(response, 0, sizeof(response));
    info = make_request("scan=info&kind=tags&object_id=OBJ_0001&session_id=S000001", NULL, 0U, response, sizeof(response));
    assert(web_scan_store_try_handle(&info, &handled) == ESP_OK && handled);
    assert(strstr(response, "ESP_ERR_NOT_FOUND") != NULL);

    /* Deleting the scan removes its tag positions with the rest of what hangs on it (and any leftover temp/backup). */
    assert(save_kind("scan=save&kind=tags&object_id=OBJ_0001&session_id=S000001", tags, tags_size));
    write_bytes(tags_temp, tags, tags_size);
    write_bytes(tags_backup, tags, tags_size);
    assert(save_kind("scan=delete&object_id=OBJ_0001&session_id=S000001", NULL, 0U));
    assert(!path_exists(tags_path) && !path_exists(tags_temp) && !path_exists(tags_backup));
    remove_layout();
}

static void test_tags_must_name_the_current_scan_and_keep_the_contract(void)
{
    make_layout();
    static const unsigned char scan[] = {31U, 32U, 33U};
    static const unsigned char other_scan[] = {41U, 42U};
    unsigned char tags[1024];
    char scan_id[20], other_id[20];
    make_scan_id(reference_crc32(scan, sizeof(scan)), scan_id);
    make_scan_id(reference_crc32(other_scan, sizeof(other_scan)), other_id);
    const char *tags_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/tag-positions-v1.json";
    char response[512] = {0};
    bool handled = false;
    assert(save_kind("scan=save&object_id=OBJ_0001&session_id=S000001", scan, sizeof(scan)));

    /* Unbound (the phone did not know the scan): refused like an unbound registration. */
    size_t size = make_tags_json(tags, sizeof(tags), "null", NULL);
    httpd_req_t unbound = make_request("scan=save&kind=tags&object_id=OBJ_0001&session_id=S000001", tags, size, response, sizeof(response));
    assert(web_scan_store_try_handle(&unbound, &handled) == ESP_OK && handled);
    assert(unbound.response_status != NULL && strcmp(unbound.response_status, "409 Conflict") == 0);
    assert(strstr(response, "registration_unbound") != NULL && !path_exists(tags_path));

    /* Bound to another scan (a late upload for a replaced scan): refused, nothing stored. */
    size = make_tags_json_for(tags, sizeof(tags), other_id);
    memset(response, 0, sizeof(response));
    httpd_req_t stale = make_request("scan=save&kind=tags&object_id=OBJ_0001&session_id=S000001", tags, size, response, sizeof(response));
    assert(web_scan_store_try_handle(&stale, &handled) == ESP_OK && handled);
    assert(stale.response_status != NULL && strcmp(stale.response_status, "409 Conflict") == 0);
    assert(!path_exists(tags_path));

    /* Valid JSON that breaks the contract the localization reads it under is refused to the phone, not stored to be ignored later. */
    static const char *const broken[] = {
        "{\"node_id\":0,\"tag_id\":0,\"x_mm\":1,\"y_mm\":0,\"z_mm\":0,\"uncertainty_mm\":0,\"confirmed\":true,\"observations\":1,\"views\":1},"
            "{\"node_id\":1,\"tag_id\":1,\"x_mm\":500,\"y_mm\":0,\"z_mm\":0,\"uncertainty_mm\":5,\"confirmed\":true,\"observations\":1,\"views\":1},"
            "{\"node_id\":2,\"tag_id\":2,\"x_mm\":0,\"y_mm\":800,\"z_mm\":0,\"uncertainty_mm\":5,\"confirmed\":true,\"observations\":1,\"views\":1}",   /* MAIN off the origin */
        "{\"node_id\":0,\"tag_id\":0,\"x_mm\":0,\"y_mm\":0,\"z_mm\":0,\"uncertainty_mm\":0,\"confirmed\":true,\"observations\":1,\"views\":1},"
            "{\"node_id\":1,\"tag_id\":1,\"x_mm\":500,\"y_mm\":0,\"z_mm\":0,\"uncertainty_mm\":5,\"confirmed\":true,\"observations\":1,\"views\":1}",   /* two nodes only */
        "{\"node_id\":0,\"tag_id\":0,\"x_mm\":0,\"y_mm\":0,\"z_mm\":0,\"uncertainty_mm\":0,\"confirmed\":true,\"observations\":1,\"views\":1},"
            "{\"node_id\":1,\"tag_id\":3,\"x_mm\":500,\"y_mm\":0,\"z_mm\":0,\"uncertainty_mm\":5,\"confirmed\":true,\"observations\":1,\"views\":1},"
            "{\"node_id\":2,\"tag_id\":2,\"x_mm\":0,\"y_mm\":800,\"z_mm\":0,\"uncertainty_mm\":5,\"confirmed\":true,\"observations\":1,\"views\":1}",   /* tag of another node */
    };
    char quoted[40];
    assert(snprintf(quoted, sizeof(quoted), "\"%s\"", scan_id) > 0);
    for (size_t i = 0U; i < sizeof(broken) / sizeof(broken[0]); ++i) {
        size = make_tags_json(tags, sizeof(tags), quoted, broken[i]);
        assert(!save_kind("scan=save&kind=tags&object_id=OBJ_0001&session_id=S000001", tags, size));
        assert(!path_exists(tags_path));
    }
    /* Not JSON at all, and a JSON text without the contract's own keys. */
    static const unsigned char junk[] = "not json";
    assert(!save_kind("scan=save&kind=tags&object_id=OBJ_0001&session_id=S000001", junk, sizeof(junk) - 1U));
    static const unsigned char thin[] = "{\"schema\":1}";
    assert(!save_kind("scan=save&kind=tags&object_id=OBJ_0001&session_id=S000001", thin, sizeof(thin) - 1U));
    assert(!path_exists(tags_path));

    /* The contract's size limit is 4096 bytes. */
    static unsigned char oversized[4097] = {0U};
    memset(response, 0, sizeof(response));
    httpd_req_t too_large = make_request("scan=save&kind=tags&object_id=OBJ_0001&session_id=S000001", oversized, sizeof(oversized), response, sizeof(response));
    assert(web_scan_store_try_handle(&too_large, &handled) == ESP_OK && handled);
    assert(too_large.response_status != NULL && strcmp(too_large.response_status, "400 Bad Request") == 0);
    assert(strstr(response, "ESP_ERR_INVALID_SIZE") != NULL);

    /* And a valid, bound file is accepted after all that. */
    size = make_tags_json_for(tags, sizeof(tags), scan_id);
    assert(save_kind("scan=save&kind=tags&object_id=OBJ_0001&session_id=S000001", tags, size));
    assert(path_exists(tags_path));
    remove_layout();
}

static void test_a_replaced_scan_takes_its_tag_positions_with_it(void)
{
    make_layout();
    static const unsigned char scan_a[] = {51U, 52U, 53U};
    static const unsigned char scan_b[] = {61U, 62U, 63U, 64U};
    unsigned char tags[1024];
    char id_a[20];
    make_scan_id(reference_crc32(scan_a, sizeof(scan_a)), id_a);
    const size_t size = make_tags_json_for(tags, sizeof(tags), id_a);
    const char *tags_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/tag-positions-v1.json";
    assert(save_kind("scan=save&object_id=OBJ_0001&session_id=S000001", scan_a, sizeof(scan_a)));
    assert(save_kind("scan=save&kind=tags&object_id=OBJ_0001&session_id=S000001", tags, size));
    assert(path_exists(tags_path));
    assert(save_kind("scan=save&object_id=OBJ_0001&session_id=S000001", scan_b, sizeof(scan_b)));
    assert(!path_exists(tags_path));       /* positions seen in the old scan are not carried into the new one */
    remove_layout();
}

static void test_tags_follow_the_staging_and_recording_rules_of_the_registration(void)
{
    make_layout();
    static const unsigned char scan[] = {71U, 72U, 73U};
    unsigned char tags[1024];
    char scan_id[20];
    make_scan_id(reference_crc32(scan, sizeof(scan)), scan_id);
    const size_t size = make_tags_json_for(tags, sizeof(tags), scan_id);
    char response[512] = {0};
    bool handled = false;

    /* Staging: stored beside the staged scan and cleared with it when the object changes. */
    assert(save_kind("scan=save&object_id=OBJ_0001&session_id=staging", scan, sizeof(scan)));
    assert(save_kind("scan=save&kind=tags&object_id=OBJ_0001&session_id=staging", tags, size));
    char final_path[WEB_SCAN_PATH_SIZE], temp_path[WEB_SCAN_PATH_SIZE], backup_path[WEB_SCAN_PATH_SIZE];
    assert(web_scan_paths("OBJ_0001", "staging", &WEB_SCAN_KIND_TAGS, final_path, temp_path, backup_path) == ESP_OK);
    assert(path_exists(final_path));
    assert(web_scan_store_clear_staging("OBJ_0001") == ESP_OK);
    assert(!path_exists(final_path) && !path_exists(temp_path) && !path_exists(backup_path));

    /* A session that is recording does not take new tag positions (staging is exempt, another session is not affected). */
    assert(save_kind("scan=save&object_id=OBJ_0001&session_id=S000001", scan, sizeof(scan)));
    assert(storage_bulk_admission_enter_protected() == ESP_OK);
    s_recorder_snapshot.running = true;
    (void)snprintf(s_recorder_snapshot.object_id, sizeof(s_recorder_snapshot.object_id), "OBJ_0001");
    (void)snprintf(s_recorder_snapshot.session_id, sizeof(s_recorder_snapshot.session_id), "S000001");
    const char *query = "scan=save&kind=tags&object_id=OBJ_0001&session_id=S000001";
    httpd_req_t save = make_request(query, tags, size, response, sizeof(response));
    assert(web_scan_store_try_handle(&save, &handled) == ESP_OK && handled);
    assert(save.response_status != NULL && strcmp(save.response_status, "409 Conflict") == 0);
    assert(strstr(response, "this session is recording") != NULL);
    memset(response, 0, sizeof(response));
    httpd_req_t remove_request = make_request("scan=delete&kind=tags&object_id=OBJ_0001&session_id=S000001", NULL, 0U, response, sizeof(response));
    assert(web_scan_store_try_handle(&remove_request, &handled) == ESP_OK && handled);
    assert(remove_request.response_status != NULL && strcmp(remove_request.response_status, "409 Conflict") == 0);
    s_recorder_snapshot.running = false;
    storage_bulk_admission_exit_protected();
    assert(save_kind(query, tags, size));
    remove_layout();
}

static void test_unknown_kind_is_still_refused(void)
{
    make_layout();
    char response[512] = {0};
    bool handled = false;
    httpd_req_t request = make_request("scan=info&kind=tag&object_id=OBJ_0001&session_id=S000001", NULL, 0U, response, sizeof(response));
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
    assert(request.response_status != NULL && strcmp(request.response_status, "400 Bad Request") == 0);
    memset(response, 0, sizeof(response));
    request = make_request("scan=info&kind=tagsx&object_id=OBJ_0001&session_id=S000001", NULL, 0U, response, sizeof(response));
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
    assert(request.response_status != NULL && strcmp(request.response_status, "400 Bad Request") == 0);
    remove_layout();
}

static void test_recording_admission_truth_table(void)
{
    make_layout();
    const unsigned char prior_scan[] = {1U, 3U, 5U};
    const unsigned char prior_occupancy[] = {2U, 4U, 6U};
    const unsigned char prior_preview[] = {7U, 8U, 9U};
    const unsigned char registration[] = "{\"valid\":true}";
    const unsigned char save_body[] = {9U, 8U};
    const char *scan_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/scan-v1.bin";
    const char *scan_temp = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/scan-v1.bin.tmp";
    const char *occupancy_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/occupancy-v1.bin";
    const char *occupancy_temp = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/occupancy-v1.bin.tmp";
    const char *preview_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/preview-v1.bin";
    const char *preview_temp = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/preview-v1.bin.tmp";
    const char *registration_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/registration-v1.json";

    write_bytes(scan_path, prior_scan, sizeof(prior_scan));
    write_bytes(occupancy_path, prior_occupancy, sizeof(prior_occupancy));
    write_bytes(preview_path, prior_preview, sizeof(prior_preview));
    write_bytes(registration_path, registration, sizeof(registration) - 1U);
    s_session_list_calls = 0U;
    assert(storage_bulk_admission_enter_protected() == ESP_OK);

    const struct {
        const char *kind;
        const char *file;
        const char *temp;
    } kinds[] = {
        {"scan", scan_path, scan_temp},
        {"occupancy", occupancy_path, occupancy_temp},
        {"preview", preview_path, preview_temp},
    };
    const struct {
        const char *verb;
        const char *action;
    } verbs[] = {
        {"save", "scan_save"},
        {"load", "scan_load"},
        {"delete", "scan_delete"},
    };

    for (size_t kind_index = 0U; kind_index < sizeof(kinds) / sizeof(kinds[0]); ++kind_index) {
        assert(path_exists(kinds[kind_index].file));
        assert(!path_exists(kinds[kind_index].temp));
        for (size_t verb_index = 0U; verb_index < sizeof(verbs) / sizeof(verbs[0]); ++verb_index) {
            char query[160];
            const int written = snprintf(
                query,
                sizeof(query),
                "scan=%s&kind=%s&object_id=OBJ_0001&session_id=S000001",
                verbs[verb_index].verb,
                kinds[kind_index].kind
            );
            assert(written > 0 && (size_t)written < sizeof(query));
            assert_recording_busy(
                query,
                strcmp(verbs[verb_index].verb, "save") == 0 ? save_body : NULL,
                strcmp(verbs[verb_index].verb, "save") == 0 ? sizeof(save_body) : 0U,
                verbs[verb_index].action
            );
            assert(path_exists(kinds[kind_index].file));
            assert(!path_exists(kinds[kind_index].temp));
        }
    }

    /* Repeating a rejected request stays before session listing and SD access. */
    assert_recording_busy(
        "scan=save&object_id=OBJ_0001&session_id=S000001",
        save_body,
        sizeof(save_body),
        "scan_save"
    );

    /* info stays readable while admission is protected. */
    for (size_t kind_index = 0U; kind_index < sizeof(kinds) / sizeof(kinds[0]); ++kind_index) {
        char response[512] = {0};
        char query[160];
        const int written = snprintf(
            query,
            sizeof(query),
            "scan=info&kind=%s&object_id=OBJ_0001&session_id=S000001",
            kinds[kind_index].kind
        );
        assert(written > 0 && (size_t)written < sizeof(query));
        httpd_req_t request = make_request(query, NULL, 0U, response, sizeof(response));
        bool handled = false;
        const unsigned before_session_checks = s_session_list_calls;
        s_storage_begin_calls = 0U;
        s_storage_end_calls = 0U;
        assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
        assert(request.response_status == NULL);
        assert(strstr(response, "\"ok\":true") != NULL);
        assert(s_session_list_calls == before_session_checks + 1U);
        assert(s_storage_begin_calls > 0U);
        assert(s_storage_begin_calls == s_storage_end_calls);
    }

    /* Registration remains governed by the existing session-scoped guard. */
    memset(&s_recorder_snapshot, 0, sizeof(s_recorder_snapshot));
    s_recorder_snapshot.running = true;
    (void)snprintf(s_recorder_snapshot.object_id, sizeof(s_recorder_snapshot.object_id), "OBJ_0001");
    (void)snprintf(s_recorder_snapshot.session_id, sizeof(s_recorder_snapshot.session_id), "S000001");
    {
        char response[512] = {0};
        httpd_req_t request = make_request(
            "scan=save&kind=registration&object_id=OBJ_0001&session_id=S000001",
            registration,
            sizeof(registration) - 1U,
            response,
            sizeof(response)
        );
        bool handled = false;
        assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
        assert(request.response_status != NULL && strcmp(request.response_status, "409 Conflict") == 0);
        assert(strstr(response, "this session is recording") != NULL);
        assert(strstr(response, "busy_reason") == NULL);
    }
    {
        char response[512] = {0};
        httpd_req_t request = make_request(
            "scan=load&kind=registration&object_id=OBJ_0001&session_id=S000001",
            NULL,
            0U,
            response,
            sizeof(response)
        );
        bool handled = false;
        assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
        assert(request.response_status == NULL);
        assert(request.response_length == sizeof(registration) - 1U);
        assert(memcmp(response, registration, sizeof(registration) - 1U) == 0);
    }

    storage_bulk_admission_exit_protected();
    {
        char response[512] = {0};
        httpd_req_t request = make_request(
            "scan=save&object_id=OBJ_0001&session_id=S000001",
            save_body,
            sizeof(save_body),
            response,
            sizeof(response)
        );
        bool handled = false;
        assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
        assert(request.response_status == NULL);
        assert(strstr(response, "\"ok\":true") != NULL);
        assert(s_storage_begin_calls == s_storage_end_calls);
    }

    remove_layout();
}


static void test_mid_transfer_admission_aborts_save_and_preserves_final(void)
{
    make_layout();
    const char *final_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/scan-v1.bin";
    const char *temp_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/scan-v1.bin.tmp";
    static const unsigned char prior[] = {0x11U, 0x22U, 0x33U, 0x44U};
    const size_t body_length = 2U * WEB_SCAN_IO_BUFFER_SIZE + 17U;
    unsigned char *body = malloc(body_length);
    assert(body != NULL);
    memset(body, 0xa5, body_length);
    write_bytes(final_path, prior, sizeof(prior));

    char response[512] = {0};
    httpd_req_t request = make_request(
        "scan=save&object_id=OBJ_0001&session_id=S000001",
        body,
        body_length,
        response,
        sizeof(response)
    );
    bool handled = false;
    s_storage_begin_calls = 0U;
    s_storage_end_calls = 0U;
    s_recv_calls = 0U;
    s_enter_after_recv_call = 2U;
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
    assert(request.response_status != NULL && strcmp(request.response_status, "409 Conflict") == 0);
    assert(strcmp(response, "{\"ok\":false,\"action\":\"scan_save\",\"busy_reason\":\"recording_active\"}") == 0);
    assert(request.body_offset == body_length);
    assert(!path_exists(temp_path));
    assert(storage_bulk_admission_is_protected());
    assert(s_storage_begin_calls == s_storage_end_calls);

    FILE *final_file = fopen(final_path, "rb");
    assert(final_file != NULL);
    unsigned char actual[sizeof(prior)];
    assert(fread(actual, 1U, sizeof(actual), final_file) == sizeof(actual));
    assert(fgetc(final_file) == EOF);
    assert(fclose(final_file) == 0);
    assert(memcmp(actual, prior, sizeof(prior)) == 0);

    storage_bulk_admission_exit_protected();
    free(body);
    remove_layout();
}


static void test_mid_transfer_preview_admission_abort_preserves_final(void)
{
    make_layout();
    const char *final_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/preview-v1.bin";
    const char *temp_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/preview-v1.bin.tmp";
    static const unsigned char prior[] = {0x31U, 0x42U, 0x53U};
    const size_t body_length = 2U * WEB_SCAN_IO_BUFFER_SIZE + 17U;
    unsigned char *body = malloc(body_length);
    assert(body != NULL);
    memset(body, 0xc3, body_length);
    write_bytes(final_path, prior, sizeof(prior));

    char response[512] = {0};
    httpd_req_t request = make_request(
        "scan=save&kind=preview&object_id=OBJ_0001&session_id=S000001",
        body,
        body_length,
        response,
        sizeof(response)
    );
    bool handled = false;
    s_storage_begin_calls = 0U;
    s_storage_end_calls = 0U;
    s_recv_calls = 0U;
    s_enter_after_recv_call = 2U;
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
    assert(request.response_status != NULL && strcmp(request.response_status, "409 Conflict") == 0);
    assert(strcmp(response, "{\"ok\":false,\"action\":\"scan_save\",\"busy_reason\":\"recording_active\"}") == 0);
    assert(request.body_offset == body_length);
    assert(!path_exists(temp_path));
    assert(storage_bulk_admission_is_protected());
    assert(s_storage_begin_calls == s_storage_end_calls);

    FILE *final_file = fopen(final_path, "rb");
    assert(final_file != NULL);
    unsigned char actual[sizeof(prior)];
    assert(fread(actual, 1U, sizeof(actual), final_file) == sizeof(actual));
    assert(fgetc(final_file) == EOF);
    assert(fclose(final_file) == 0);
    assert(memcmp(actual, prior, sizeof(prior)) == 0);

    storage_bulk_admission_exit_protected();
    free(body);
    remove_layout();
}


static void test_mid_stream_admission_aborts_load(void)
{
    make_layout();
    const char *final_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/scan-v1.bin";
    const size_t file_size = 2U * WEB_SCAN_IO_BUFFER_SIZE + 19U;
    unsigned char *file_bytes = malloc(file_size);
    char *response = malloc(file_size);
    assert(file_bytes != NULL && response != NULL);
    for (size_t i = 0U; i < file_size; ++i) file_bytes[i] = (unsigned char)(i & 0xffU);
    write_bytes(final_path, file_bytes, file_size);

    httpd_req_t request = make_request(
        "scan=load&object_id=OBJ_0001&session_id=S000001",
        NULL,
        0U,
        response,
        file_size
    );
    bool handled = false;
    s_storage_begin_calls = 0U;
    s_storage_end_calls = 0U;
    s_chunked_response_bytes = 0U;
    s_send_chunk_calls = 0U;
    s_enter_after_send_chunk = 1U;
    s_chunked_response_terminated = false;
    assert(web_scan_store_try_handle(&request, &handled) == ESP_ERR_INVALID_STATE && handled);
    assert(request.response_status == NULL);
    assert(request.response_length == WEB_SCAN_IO_BUFFER_SIZE);
    assert(s_chunked_response_bytes == WEB_SCAN_IO_BUFFER_SIZE);
    assert(memcmp(response, file_bytes, WEB_SCAN_IO_BUFFER_SIZE) == 0);
    assert(!s_chunked_response_terminated);
    assert(storage_bulk_admission_is_protected());
    assert(s_storage_begin_calls == s_storage_end_calls);

    storage_bulk_admission_exit_protected();
    free(response);
    free(file_bytes);
    remove_layout();
}


static void test_write_failure_removes_temp_and_balances_storage_guard(void)
{
    make_layout();
    const char *final_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/scan-v1.bin";
    const char *temp_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/scan-v1.bin.tmp";
    static const unsigned char prior[] = {0x91U, 0x92U, 0x93U};
    const size_t body_length = 2U * WEB_SCAN_IO_BUFFER_SIZE + 3U;
    unsigned char *body = malloc(body_length);
    assert(body != NULL);
    memset(body, 0x5a, body_length);
    write_bytes(final_path, prior, sizeof(prior));

    char response[512] = {0};
    httpd_req_t request = make_request(
        "scan=save&object_id=OBJ_0001&session_id=S000001",
        body,
        body_length,
        response,
        sizeof(response)
    );
    bool handled = false;
    s_storage_begin_calls = 0U;
    s_storage_end_calls = 0U;
    s_recv_calls = 0U;
    s_fwrite_calls = 0U;
    s_fail_fwrite_call = 2U;
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
    s_fail_fwrite_call = 0U;
    assert(strstr(response, "ESP_ERR") != NULL);
    assert(!path_exists(temp_path));
    assert(s_storage_begin_calls == s_storage_end_calls);

    FILE *final_file = fopen(final_path, "rb");
    assert(final_file != NULL);
    unsigned char actual[sizeof(prior)];
    assert(fread(actual, 1U, sizeof(actual), final_file) == sizeof(actual));
    assert(fgetc(final_file) == EOF);
    assert(fclose(final_file) == 0);
    assert(memcmp(actual, prior, sizeof(prior)) == 0);

    free(body);
    remove_layout();
}


static void test_recovery_matrix_validates_every_artifact_kind(void)
{
    static const unsigned char bad_bytes[] = {'b', 'a', 'd'};
    static const unsigned char temp_bytes[] = {'t', 'm', 'p'};
    static const unsigned char registration_final[] = "{\"schema\":1}";
    static const unsigned char registration_backup[] = "{\"schema\":1,\"generation\":2}";
    char scan_id_a[20];
    char scan_id_b[20];
    make_scan_id(UINT32_C(0x11111111), scan_id_a);
    make_scan_id(UINT32_C(0x22222222), scan_id_b);
    unsigned char scan_final[64], scan_backup[64];
    unsigned char occupancy_final[256], occupancy_backup[256];
    unsigned char preview_final[64], preview_backup[64];
    const size_t scan_final_size = make_scan_artifact(scan_final, sizeof(scan_final), 0xa1U);
    const size_t scan_backup_size = make_scan_artifact(scan_backup, sizeof(scan_backup), 0xb2U);
    const size_t occupancy_final_size = make_occupancy_header(occupancy_final, sizeof(occupancy_final), scan_id_a);
    const size_t occupancy_backup_size = make_occupancy_header(occupancy_backup, sizeof(occupancy_backup), scan_id_b);
    const size_t preview_final_size = make_preview_header(preview_final, sizeof(preview_final), UINT32_C(0x11111111));
    const size_t preview_backup_size = make_preview_header(preview_backup, sizeof(preview_backup), UINT32_C(0x22222222));
    const struct {
        const char *name;
        const web_scan_kind_t *kind;
        const unsigned char *final_bytes;
        size_t final_size;
        const unsigned char *backup_bytes;
        size_t backup_size;
    } cases[] = {
        {"scan", &WEB_SCAN_KIND_SCAN, scan_final, scan_final_size, scan_backup, scan_backup_size},
        {"occupancy", &WEB_SCAN_KIND_OCCUPANCY, occupancy_final, occupancy_final_size, occupancy_backup, occupancy_backup_size},
        {"preview", &WEB_SCAN_KIND_PREVIEW, preview_final, preview_final_size, preview_backup, preview_backup_size},
        {"registration", &WEB_SCAN_KIND_REGISTRATION, registration_final, sizeof(registration_final) - 1U, registration_backup, sizeof(registration_backup) - 1U},
    };

    for (size_t kind_index = 0U; kind_index < sizeof(cases) / sizeof(cases[0]); ++kind_index) {
        for (size_t state = 0U; state < 4U; ++state) {
            make_layout();
            char final_path[WEB_SCAN_PATH_SIZE], temp_path[WEB_SCAN_PATH_SIZE], backup_path[WEB_SCAN_PATH_SIZE];
            assert(web_scan_paths("OBJ_0001", "S000001", cases[kind_index].kind, final_path, temp_path, backup_path) == ESP_OK);
            if (state != 2U) {
                const unsigned char *final_bytes = state == 0U ? cases[kind_index].final_bytes : bad_bytes;
                const size_t final_size = state == 0U ? cases[kind_index].final_size : sizeof(bad_bytes);
                write_bytes(final_path, final_bytes, final_size);
            }
            const unsigned char *backup_bytes = state == 3U ? bad_bytes : cases[kind_index].backup_bytes;
            const size_t backup_size = state == 3U ? sizeof(bad_bytes) : cases[kind_index].backup_size;
            write_bytes(backup_path, backup_bytes, backup_size);
            write_bytes(temp_path, temp_bytes, sizeof(temp_bytes));

            char response[512] = {0};
            httpd_req_t info_request;
            request_info_kind(cases[kind_index].name, response, sizeof(response), &info_request);
            if (state == 3U) {
                assert(strstr(response, "\"ok\":false") != NULL);
                assert(file_matches_bytes(final_path, bad_bytes, sizeof(bad_bytes)));
                assert(file_matches_bytes(backup_path, bad_bytes, sizeof(bad_bytes)));
                assert(file_matches_bytes(temp_path, temp_bytes, sizeof(temp_bytes)));
            }
            else {
                const unsigned char *expected_final = state == 0U
                    ? cases[kind_index].final_bytes
                    : cases[kind_index].backup_bytes;
                const size_t expected_size = state == 0U
                    ? cases[kind_index].final_size
                    : cases[kind_index].backup_size;
                assert(strstr(response, "\"ok\":true") != NULL);
                assert(file_matches_bytes(final_path, expected_final, expected_size));
                assert(!path_exists(backup_path));
                assert(!path_exists(temp_path));
            }
            remove_layout();
        }
    }
}


static void test_scan_recovery_keeps_derivative_generation_distinct(void)
{
    make_layout();
    unsigned char recovered_scan[64];
    const size_t recovered_size = make_scan_artifact(recovered_scan, sizeof(recovered_scan), 0x31U);
    unsigned char invalid_scan[64];
    memcpy(invalid_scan, recovered_scan, recovered_size);
    invalid_scan[0] = (unsigned char)'X';
    static const unsigned char other_scan[] = {8U, 6U, 4U, 2U};
    char other_scan_id[20];
    make_scan_id(reference_crc32(other_scan, sizeof(other_scan)), other_scan_id);
    unsigned char occupancy[256];
    const size_t occupancy_size = make_occupancy_header(occupancy, sizeof(occupancy), other_scan_id);
    const char *scan_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/scan-v1.bin";
    const char *scan_backup = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/scan-v1.bin.bak";
    const char *occupancy_path = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/occupancy-v1.bin";
    write_bytes(scan_path, invalid_scan, recovered_size);
    write_bytes(scan_backup, recovered_scan, recovered_size);
    write_bytes(occupancy_path, occupancy, occupancy_size);
    web_scan_identity_cache_set_locked(scan_path, recovered_size, reference_crc32(invalid_scan, recovered_size));

    char scan_info[512] = {0};
    httpd_req_t request;
    request_info_kind("scan", scan_info, sizeof(scan_info), &request);
    char recovered_id[20];
    make_scan_id(reference_crc32(recovered_scan, recovered_size), recovered_id);
    assert(strstr(scan_info, recovered_id) != NULL);
    assert(file_matches_bytes(scan_path, recovered_scan, recovered_size));
    assert(!path_exists(scan_backup));

    char occupancy_info[512] = {0};
    request_info_kind("occupancy", occupancy_info, sizeof(occupancy_info), &request);
    assert(strstr(occupancy_info, other_scan_id) != NULL);
    assert(strcmp(recovered_id, other_scan_id) != 0);
    remove_layout();
}

int main(void)
{
    assert(storage_bulk_admission_initialize() == ESP_OK);
    test_kind_size_validation_boundaries();
    test_scan_and_occupancy_size_limits();
    test_scan_save_and_chunked_load_round_trip();
    test_scan_rejects_unknown_session_before_file_access();
    test_occupancy_kind_is_stored_beside_the_scan();
    test_scan_replacement_invalidates_derivatives_and_returns_crc();
    test_scan_replacement_delete_failure_keeps_old_scan();
    test_deleting_a_shared_scan_unlinks_only_this_sessions_reference();
    test_registration_is_bound_to_the_scan_it_was_saved_for();
    test_scan_replacement_power_loss_ordering();
    test_derivative_compare_and_commit_rejects_stale_uploads();
    test_derivative_compare_and_commit_toctou_recheck();
    test_identity_cache_publish_preserves_replacement();
    test_identity_cache_store_reports_a_scan_that_vanished();
    test_registration_unbound_is_rejected();
    test_registration_round_trip_bounds_and_session_validation();
    test_registration_admission_parses_the_root_object();
    test_tags_round_trip_is_json_bound_and_removed_with_its_scan();
    test_tags_must_name_the_current_scan_and_keep_the_contract();
    test_a_replaced_scan_takes_its_tag_positions_with_it();
    test_tags_follow_the_staging_and_recording_rules_of_the_registration();
    test_unknown_kind_is_still_refused();
    test_recording_admission_truth_table();
    test_mid_transfer_admission_aborts_save_and_preserves_final();
    test_mid_transfer_preview_admission_abort_preserves_final();
    test_mid_stream_admission_aborts_load();
    test_write_failure_removes_temp_and_balances_storage_guard();
    test_recovery_matrix_validates_every_artifact_kind();
    test_scan_recovery_keeps_derivative_generation_distinct();
    test_registration_recording_guard_is_session_scoped();
    test_staging_round_trip_kinds_and_limits();
    test_staging_accepts_only_reserved_literal_and_safe_object_ids();
    test_staging_operations_return_409_while_protected();
    test_object_change_hook_clears_all_staged_files();
    return 0;
}
