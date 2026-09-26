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
static bool s_chunked_response_terminated;
static seismic_recorder_snapshot_t s_recorder_snapshot;

size_t web_scan_store_test_fwrite(
    const void *buffer,
    size_t size,
    size_t count,
    FILE *file
);

static void make_directory(const char *path)
{
    assert(mkdir(path, 0775) == 0 || errno == EEXIST);
}

static void make_layout(void)
{
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
#ifdef _WIN32
    (void)_rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001");
    (void)_rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions");
    (void)_rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001");
    (void)_rmdir(STORAGE_MANAGER_OBJECTS_PATH);
    (void)_rmdir(STORAGE_MANAGER_ROOT_PATH);
    (void)_rmdir(STORAGE_MANAGER_MOUNT_POINT);
#else
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001");
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions");
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001");
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH);
    (void)rmdir(STORAGE_MANAGER_ROOT_PATH);
    (void)rmdir(STORAGE_MANAGER_MOUNT_POINT);
#endif
    memset(&s_recorder_snapshot, 0, sizeof(s_recorder_snapshot));
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
    return ESP_OK;
}

void storage_manager_operation_end(void)
{
    s_storage_end_calls++;
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
#include "../../components/web_server/web_scan_store.c"
#undef fwrite
#undef static

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

static void test_scan_save_and_chunked_load_round_trip(void)
{
    make_layout();
    s_storage_begin_calls = 0U;
    s_storage_end_calls = 0U;
    s_priority_changes = 0U;

    const unsigned char artifact[] = {0U, 1U, 2U, 255U, 8U, 13U, 21U};
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

    memset(response, 0, sizeof(response));
    request = make_request(
        "scan=save&kind=preview&object_id=OBJ_0001&session_id=S000001",
        body,
        WEB_SCAN_KIND_PREVIEW.max_bytes,
        response,
        sizeof(response)
    );
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
    assert(strstr(response, "\"ok\":true") != NULL);
    assert(request.body_offset == WEB_SCAN_KIND_PREVIEW.max_bytes);

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

    memset(response, 0, sizeof(response));
    request = make_request(
        "scan=save&kind=occupancy&object_id=OBJ_0001&session_id=S000001",
        body,
        occupancy_limit,
        response,
        sizeof(response)
    );
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK && handled);
    assert(strstr(response, "\"ok\":true") != NULL);
    assert(request.body_offset == occupancy_limit);

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
    const unsigned char occupancy[] = {'G', 'O', 'C', 'C', 9U};
    const unsigned char preview[] = {'G', 'X', 'P', 'R', 'V', '1', 1U};
    assert(save_kind("scan=save&object_id=OBJ_0001&session_id=S000001", scan, sizeof(scan)));
    assert(save_kind("scan=save&kind=occupancy&object_id=OBJ_0001&session_id=S000001", occupancy, sizeof(occupancy)));
    assert(save_kind("scan=save&kind=preview&object_id=OBJ_0001&session_id=S000001", preview, sizeof(preview)));

    char response[512] = {0};
    assert(load_kind("scan=load&kind=occupancy&object_id=OBJ_0001&session_id=S000001", response, sizeof(response)) == sizeof(occupancy));
    assert(memcmp(response, occupancy, sizeof(occupancy)) == 0);
    memset(response, 0, sizeof(response));
    assert(load_kind("scan=load&kind=preview&object_id=OBJ_0001&session_id=S000001", response, sizeof(response)) == sizeof(preview));
    assert(memcmp(response, preview, sizeof(preview)) == 0);
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
        memset(info, 0, sizeof(info));
        info_request = make_request(
            "scan=info&kind=occupancy&object_id=OBJ_0001&session_id=S000001", NULL, 0U, info, sizeof(info));
        assert(web_scan_store_try_handle(&info_request, &info_handled) == ESP_OK);
        assert(strstr(info, "\"bytes\":5") != NULL);
        memset(info, 0, sizeof(info));
        info_request = make_request(
            "scan=info&kind=preview&object_id=OBJ_0001&session_id=S000001", NULL, 0U, info, sizeof(info));
        assert(web_scan_store_try_handle(&info_request, &info_handled) == ESP_OK);
        assert(strstr(info, "\"bytes\":7") != NULL);
    }

    /* Deleting the scan also removes its occupancy and preview derivatives. */
    assert(save_kind("scan=delete&object_id=OBJ_0001&session_id=S000001", NULL, 0U));
    {
        char info[512] = {0};
        httpd_req_t info_request = make_request(
            "scan=info&object_id=OBJ_0001&session_id=S000001", NULL, 0U, info, sizeof(info));
        bool info_handled = false;
        assert(web_scan_store_try_handle(&info_request, &info_handled) == ESP_OK);
        assert(strstr(info, "ESP_ERR_NOT_FOUND") != NULL);
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

static void test_registration_round_trip_bounds_and_session_validation(void)
{
    make_layout();
    const unsigned char registration[] = "{\"schema\":1,\"source\":\"apriltag\"}";
    const char *registration_kind = "kind=registration&";
    char query[160];
    char response[512] = {0};
    (void)snprintf(query, sizeof(query), "scan=save&%sobject_id=OBJ_0001&session_id=S000001", registration_kind);
    assert(save_kind(query, registration, sizeof(registration) - 1U));

    memset(response, 0, sizeof(response));
    httpd_req_t load = make_request(
        "scan=load&kind=registration&object_id=OBJ_0001&session_id=S000001", NULL, 0U, response, sizeof(response));
    bool handled = false;
    assert(web_scan_store_try_handle(&load, &handled) == ESP_OK && handled);
    assert(load.content_type != NULL && strcmp(load.content_type, "application/json; charset=utf-8") == 0);
    assert(load.response_length == sizeof(registration) - 1U);
    assert(memcmp(response, registration, sizeof(registration) - 1U) == 0);

    memset(response, 0, sizeof(response));
    httpd_req_t info = make_request(
        "scan=info&kind=registration&object_id=OBJ_0001&session_id=S000001", NULL, 0U, response, sizeof(response));
    assert(web_scan_store_try_handle(&info, &handled) == ESP_OK && handled);
    assert(strstr(response, "\"ok\":true") != NULL);
    assert(strstr(response, "\"bytes\":32") != NULL);

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
        "scan=save&kind=registration&object_id=OBJ_0001&session_id=S000002", registration, sizeof(registration) - 1U, response, sizeof(response));
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
    const unsigned char registration[] = "{\"valid\":true}";
    char response[512] = {0};
    const char *query = "scan=save&kind=registration&object_id=OBJ_0001&session_id=S000001";
    assert(save_kind(query, registration, sizeof(registration) - 1U));

    assert(storage_bulk_admission_enter_protected() == ESP_OK);

    s_recorder_snapshot.running = true;
    (void)snprintf(s_recorder_snapshot.object_id, sizeof(s_recorder_snapshot.object_id), "OBJ_0001");
    (void)snprintf(s_recorder_snapshot.session_id, sizeof(s_recorder_snapshot.session_id), "S000001");
    httpd_req_t save = make_request(query, registration, sizeof(registration) - 1U, response, sizeof(response));
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
    assert(load.response_length == sizeof(registration) - 1U);

    memset(response, 0, sizeof(response));
    httpd_req_t info = make_request(
        "scan=info&kind=registration&object_id=OBJ_0001&session_id=S000001", NULL, 0U, response, sizeof(response));
    assert(web_scan_store_try_handle(&info, &handled) == ESP_OK && handled);
    assert(strstr(response, "\"ok\":true") != NULL);

    (void)snprintf(s_recorder_snapshot.session_id, sizeof(s_recorder_snapshot.session_id), "S000002");
    memset(response, 0, sizeof(response));
    save = make_request(query, registration, sizeof(registration) - 1U, response, sizeof(response));
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

int main(void)
{
    assert(storage_bulk_admission_initialize() == ESP_OK);
    test_kind_size_validation_boundaries();
    test_scan_and_occupancy_size_limits();
    test_scan_save_and_chunked_load_round_trip();
    test_scan_rejects_unknown_session_before_file_access();
    test_occupancy_kind_is_stored_beside_the_scan();
    test_registration_round_trip_bounds_and_session_validation();
    test_recording_admission_truth_table();
    test_mid_transfer_admission_aborts_save_and_preserves_final();
    test_mid_transfer_preview_admission_abort_preserves_final();
    test_mid_stream_admission_aborts_load();
    test_write_failure_removes_temp_and_balances_storage_guard();
    test_registration_recording_guard_is_session_scoped();
    return 0;
}
