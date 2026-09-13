/*
 * Host integration contract for the recorder and web seismic boundaries.
 *
 * This uses the production seismic_buffer, seismic_recorder_fs and
 * web_seismic_api/web_json_writer code with narrow storage/http fakes. It
 * proves that one AD7606-labelled frame stream survives binary recording and
 * the HTTP JSON snapshot with its metadata and signed impulse intact.
 * Physical SD, SPI DMA, TFT, Wi-Fi and FreeRTOS scheduling remain target/HIL
 * responsibilities.
 */
#include <assert.h>
#include <errno.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
/* Keep strict -std=c11 host builds independent of libc feature-test defaults.
 * ESP-IDF exposes these POSIX compatibility functions on target. */
int fileno(FILE *stream);
size_t strnlen(const char *string, size_t max_length);
#endif

#define STORAGE_MANAGER_MOUNT_POINT "seismic_pipeline_contract_tmp"

#include "esp_err.h"
#include "esp_http_server.h"
#include "object_store.h"
#include "seismic_buffer.h"
#include "seismic_recorder_internal.h"
#include "storage_manager.h"
#include "web_server_internal.h"

static unsigned storage_begin_calls;
static unsigned storage_end_calls;
static httpd_uri_t registered_uri;
static bool fake_complete_metadata;

static int make_directory(const char *path)
{
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0775);
#endif
}

static void make_layout(void)
{
    assert(make_directory(STORAGE_MANAGER_MOUNT_POINT) == 0 || errno == EEXIST);
    assert(make_directory(STORAGE_MANAGER_ROOT_PATH) == 0 || errno == EEXIST);
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH) == 0 || errno == EEXIST);
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001") == 0 || errno == EEXIST);
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions") == 0 || errno == EEXIST);
}

static void remove_layout(void)
{
    char path[256];
    (void)snprintf(path, sizeof(path), "%s/OBJ_0001/sessions/S000001/samples.bin",
                   STORAGE_MANAGER_OBJECTS_PATH);
    (void)remove(path);
    (void)snprintf(path, sizeof(path), "%s/OBJ_0001/sessions/S000001",
                   STORAGE_MANAGER_OBJECTS_PATH);
#ifdef _WIN32
    (void)_rmdir(path);
    (void)_rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions");
    (void)_rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001");
    (void)_rmdir(STORAGE_MANAGER_OBJECTS_PATH);
    (void)_rmdir(STORAGE_MANAGER_ROOT_PATH);
    (void)_rmdir(STORAGE_MANAGER_MOUNT_POINT);
#else
    (void)rmdir(path);
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions");
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001");
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH);
    (void)rmdir(STORAGE_MANAGER_ROOT_PATH);
    (void)rmdir(STORAGE_MANAGER_MOUNT_POINT);
#endif
}

/* Production filesystem boundary fakes. */
esp_err_t storage_manager_operation_begin(void)
{
    ++storage_begin_calls;
    return ESP_OK;
}

void storage_manager_operation_end(void)
{
    ++storage_end_calls;
}

esp_err_t storage_manager_create_directory(const char *path)
{
    return make_directory(path) == 0 || errno == EEXIST ? ESP_OK : ESP_FAIL;
}

esp_err_t storage_manager_create_directory_exclusive(const char *path)
{
    errno = 0;
    if (make_directory(path) == 0 && errno != EEXIST) {
        return ESP_OK;
    }
    return errno == EEXIST ? ESP_ERR_INVALID_STATE : ESP_FAIL;
}

esp_err_t storage_manager_remove_tree(const char *path)
{
    (void)path;
    return ESP_OK;
}

esp_err_t object_store_get_profile(const char *object_id, object_store_profile_t *profile)
{
    (void)object_id;
    (void)profile;
    return ESP_OK;
}

esp_err_t seismic_recorder_json_read_summary(
    const char *object_id,
    const char *session_id,
    seismic_recorder_session_summary_t *summary
)
{
    (void)object_id;
    (void)session_id;
    if (!fake_complete_metadata || summary == NULL) {
        return ESP_FAIL;
    }
    memset(summary, 0, sizeof(*summary));
    snprintf(summary->session_id, sizeof(summary->session_id), "%s", session_id);
    snprintf(summary->source_name, sizeof(summary->source_name), "AD7606");
    summary->complete = true;
    summary->sample_rate_hz = 500U;
    summary->channel_count = SEISMIC_CHANNEL_COUNT;
    summary->samples_written = 20U;
    summary->recording_ms = 40U;
    (void)object_id;
    return ESP_OK;
}

esp_err_t seismic_recorder_json_read_summary_known_present(
    const char *object_id,
    const char *session_id,
    seismic_recorder_session_summary_t *summary
)
{
    return seismic_recorder_json_read_summary(
        object_id,
        session_id,
        summary
    );
}

/* Minimal HTTP server fake that captures the production chunked JSON body. */
size_t httpd_req_get_url_query_len(httpd_req_t *request)
{
    return request != NULL && request->query != NULL
        ? strlen(request->query)
        : 0U;
}

esp_err_t httpd_req_get_url_query_str(httpd_req_t *request, char *buffer, size_t buffer_size)
{
    if (request == NULL || request->query == NULL || buffer == NULL || buffer_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t length = strlen(request->query);
    if (length >= buffer_size) {
        return ESP_ERR_INVALID_SIZE;
    }
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
            const size_t candidate_length = (size_t)(equals - cursor);
            if (candidate_length == key_length && strncmp(cursor, key, key_length) == 0) {
                const char *start = equals + 1;
                const size_t length = end == NULL ? strlen(start) : (size_t)(end - start);
                if (length == 0U || length >= value_size) {
                    return ESP_ERR_INVALID_SIZE;
                }
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

esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type)
{
    assert(request != NULL);
    request->content_type = type;
    return ESP_OK;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *field, const char *value)
{
    assert(request != NULL);
    if (strcmp(field, "Cache-Control") == 0) {
        request->cache_control = value;
    }
    return ESP_OK;
}

esp_err_t httpd_resp_send_chunk(httpd_req_t *request, const char *buffer, ssize_t length)
{
    assert(request != NULL);
    if (buffer == NULL && length == 0) {
        return ESP_OK;
    }
    assert(buffer != NULL && length > 0);
    assert(request->response_length + (size_t)length < request->response_capacity);
    memcpy(request->response_body + request->response_length, buffer, (size_t)length);
    request->response_length += (size_t)length;
    request->response_body[request->response_length] = '\0';
    return ESP_OK;
}

esp_err_t httpd_resp_send_err(httpd_req_t *request, int status, const char *message)
{
    (void)message;
    if (request != NULL) {
        request->error_status = (unsigned)status;
    }
    return ESP_FAIL;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *uri)
{
    assert(server != NULL && uri != NULL);
    registered_uri = *uri;
    return ESP_OK;
}

/* Expose the production handler for this translation unit only. */
#define static
#include "../../components/web_server/web_seismic_api.c"
#undef static

#include "../../components/web_server/web_json_writer.c"
#include "../../components/seismic_recorder/seismic_recorder_fs.c"

static void push_pipeline_frames(size_t count)
{
    for (size_t index = 0; index < count; ++index) {
        float channels[SEISMIC_CHANNEL_COUNT] = {
            (float)(index + 1U),
            (float)(index + 2U),
            (float)(index + 3U),
            (float)(index + 4U),
        };
        if (index == 6U) {
            channels[0] = 18000.0f;
            channels[1] = -12000.0f;
        }
        assert(seismic_buffer_push((int64_t)(index + 1U) * 2000LL, channels) == ESP_OK);
    }
}

static void test_recorder_binary_and_web_json_share_the_same_frames(void)
{
    remove_layout();
    make_layout();
    storage_begin_calls = 0U;
    storage_end_calls = 0U;
    fake_complete_metadata = false;
    assert(seismic_buffer_init() == ESP_OK);
    assert(seismic_buffer_configure(SEISMIC_SOURCE_AD7606, 500U) == ESP_OK);
    push_pipeline_frames(20U);

    seismic_frame_t frames[20] = { 0 };
    size_t frame_count = 0U;
    seismic_buffer_info_t info = { 0 };
    uint64_t cursor = 0U;
    uint64_t dropped = 0U;
    assert(seismic_buffer_copy_since(&cursor, frames, 20U, &frame_count, &dropped, &info) == ESP_OK);
    assert(frame_count == 20U && dropped == 0U && cursor == 20U);
    assert(info.source == SEISMIC_SOURCE_AD7606 && info.sample_rate_hz == 500U);

    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE] = { 0 };
    FILE *samples_file = NULL;
    assert(seismic_recorder_fs_create_session("OBJ_0001", info.sample_rate_hz,
                                              SEISMIC_CHANNEL_COUNT, session_id,
                                              &samples_file) == ESP_OK);
    assert(strcmp(session_id, "S000001") == 0);
    size_t written = 0U;
    assert(seismic_recorder_fs_write_frames(samples_file, frames, frame_count, &written) == ESP_OK);
    assert(written == frame_count);
    assert(seismic_recorder_fs_close(samples_file) == ESP_OK);

    FILE *readback = fopen(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/samples.bin", "rb");
    assert(readback != NULL);
    seismic_recorder_binary_header_t header = { 0 };
    assert(fread(&header, sizeof(header), 1U, readback) == 1U);
    assert(memcmp(header.magic, "GEOSEIS1", 8U) == 0);
    assert(header.version == 1U && header.channel_count == SEISMIC_CHANNEL_COUNT);
    assert(header.sample_rate_hz == 500U && header.frame_size_bytes == sizeof(seismic_frame_t));
    seismic_frame_t recorded[20] = { 0 };
    assert(fread(recorded, sizeof(recorded[0]), 20U, readback) == 20U);
    assert(recorded[6].timestamp_us == frames[6].timestamp_us);
    assert(recorded[6].channel_uv[0] == 18000.0f);
    assert(recorded[6].channel_uv[1] == -12000.0f);
    assert(fgetc(readback) == EOF);
    assert(fclose(readback) == 0);

    fake_complete_metadata = true;
    char saved_response[32768] = { 0 };
    httpd_req_t saved_request = {
        .uri = "/api/seismic/session",
        .query = "object_id=OBJ_0001&session_id=S000001&start_frame=0&frame_count=20&points=20",
        .response_body = saved_response,
        .response_capacity = sizeof(saved_response),
    };
    assert(web_seismic_saved_handler(&saved_request) == ESP_OK);
    assert(saved_request.error_status == 0U);
    assert(strstr(saved_response, "\"object_id\":\"OBJ_0001\"") != NULL);
    assert(strstr(saved_response, "\"session_id\":\"S000001\"") != NULL);
    assert(strstr(saved_response, "\"frame_count\":20") != NULL);
    assert(strstr(saved_response, "\"min_uv\":18000.000") != NULL);
    assert(strstr(saved_response, "\"min_uv\":-12000.000") != NULL ||
           strstr(saved_response, "\"max_uv\":-12000.000") != NULL);

    char response[8192] = { 0 };
    httpd_req_t request = {
        .uri = "/api/seismic",
        .query = "points=20",
        .response_body = response,
        .response_capacity = sizeof(response),
    };
    assert(web_seismic_handler(&request) == ESP_OK);
    assert(request.error_status == 0U);
    assert(strcmp(request.content_type, "application/json; charset=utf-8") == 0);
    assert(strcmp(request.cache_control, "no-store") == 0);
    assert(strstr(response, "\"source\":\"AD7606\"") != NULL);
    assert(strstr(response, "\"sample_rate_hz\":500") != NULL);
    assert(strstr(response, "\"count\":20") != NULL);
    assert(strstr(response, "\"total_frames\":20") != NULL);
    assert(strstr(response, "\"timestamps_ms\":[2,4,6") != NULL);
    assert(strstr(response, "18000.000") != NULL);
    assert(strstr(response, "-12000.000") != NULL);

    assert(web_seismic_api_register((httpd_handle_t)&request) == ESP_OK);
    assert(strcmp(registered_uri.uri, "/api/seismic") == 0);
    assert(registered_uri.method == HTTP_GET);
    assert(registered_uri.handler == web_seismic_handler);
    assert(storage_begin_calls == storage_end_calls);
    remove_layout();
}

int main(void)
{
    test_recorder_binary_and_web_json_share_the_same_frames();
    puts("seismic recorder/web pipeline contract tests passed");
    return 0;
}

