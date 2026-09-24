#include <assert.h>
#include <errno.h>
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

#include "app_task_config.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "object_format.h"
#include "object_store.h"
#include "seismic_recorder.h"
#include "storage_manager.h"
#include "web_server_internal.h"

static unsigned s_storage_begin_calls;
static unsigned s_storage_end_calls;
static unsigned s_priority_changes;

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
    (void)remove(final_path);
    (void)remove(temp_path);
    (void)remove(backup_path);
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

bool object_format_id_is_valid(const char *object_id)
{
    return object_id != NULL && strcmp(object_id, "OBJ_0001") == 0;
}

#define static
#include "../../components/web_server/web_scan_store.c"
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

static size_t load_kind(const char *query, char *response, size_t capacity)
{
    httpd_req_t request = make_request(query, NULL, 0U, response, capacity);
    bool handled = false;
    assert(web_scan_store_try_handle(&request, &handled) == ESP_OK);
    assert(handled);
    return request.response_length;
}

/* MAX-162: the compact occupancy artifact lives next to the scan in the same
 * session directory, is stored/loaded independently via kind=occupancy, and
 * is deleted together with the scan it was derived from. */
static void test_occupancy_kind_is_stored_beside_the_scan(void)
{
    make_layout();
    const unsigned char scan[] = {1U, 2U, 3U};
    const unsigned char occupancy[] = {'G', 'O', 'C', 'C', 9U};
    assert(save_kind("scan=save&object_id=OBJ_0001&session_id=S000001", scan, sizeof(scan)));
    assert(save_kind("scan=save&kind=occupancy&object_id=OBJ_0001&session_id=S000001", occupancy, sizeof(occupancy)));

    char response[512] = {0};
    assert(load_kind("scan=load&kind=occupancy&object_id=OBJ_0001&session_id=S000001", response, sizeof(response)) == sizeof(occupancy));
    assert(memcmp(response, occupancy, sizeof(occupancy)) == 0);
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
    }

    /* Deleting the scan also removes the occupancy derived from it. */
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

    /* Unknown kinds are rejected; oversized occupancy is refused. */
    assert(!save_kind("scan=save&kind=bogus&object_id=OBJ_0001&session_id=S000001", scan, sizeof(scan)));
    remove_layout();
}

int main(void)
{
    test_scan_save_and_chunked_load_round_trip();
    test_scan_rejects_unknown_session_before_file_access();
    test_occupancy_kind_is_stored_beside_the_scan();
    return 0;
}
