#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_timer.h"
#include "web_server_internal.h"

static unsigned type_calls;
static unsigned header_calls;
static unsigned body_send_calls;
static unsigned chunk_calls;
static unsigned final_chunk_calls;
static unsigned yield_calls;
static unsigned register_calls;
static int fail_header_call;
static int fail_send_call;
static esp_err_t register_result;
static char last_type[96];
static char header_fields[8][32];
static char header_values[8][96];
static size_t sent_lengths[8];
static const unsigned char *expected_start;
static size_t payload_length;
static size_t expected_offset;

esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type)
{
    assert(request != NULL);
    assert(type != NULL);
    ++type_calls;
    snprintf(last_type, sizeof(last_type), "%s", type);
    return ESP_OK;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *field, const char *value)
{
    assert(request != NULL);
    assert(field != NULL);
    assert(value != NULL);
    ++header_calls;
    if (header_calls <= sizeof(header_fields) / sizeof(header_fields[0])) {
        snprintf(header_fields[header_calls - 1U], sizeof(header_fields[0]), "%s", field);
        snprintf(header_values[header_calls - 1U], sizeof(header_values[0]), "%s", value);
    }
    return fail_header_call == (int)header_calls ? ESP_FAIL : ESP_OK;
}

static void assert_payload(const char *buffer, ssize_t length)
{
    assert(buffer != NULL);
    assert(length > 0);
    assert((size_t)length <= payload_length - expected_offset);
    assert(memcmp(buffer, expected_start + expected_offset, (size_t)length) == 0);
    expected_offset += (size_t)length;
}

esp_err_t httpd_resp_send(httpd_req_t *request, const char *buffer, ssize_t length)
{
    assert(request != NULL);
    ++body_send_calls;
    assert(fail_send_call == 0);
    assert(length >= 0);
    if (length > 0) {
        assert_payload(buffer, length);
    }
    sent_lengths[0] = (size_t)length;
    return ESP_OK;
}

esp_err_t httpd_resp_send_chunk(httpd_req_t *request, const char *buffer, ssize_t length)
{
    assert(request != NULL);
    ++chunk_calls;
    if (buffer == NULL && length == 0) {
        ++final_chunk_calls;
        return fail_send_call == (int)chunk_calls ? ESP_FAIL : ESP_OK;
    }

    assert(length > 0);
    assert((size_t)length <= 2048U);
    if (fail_send_call == (int)chunk_calls) {
        return ESP_FAIL;
    }
    assert_payload(buffer, length);
    sent_lengths[chunk_calls - 1U] = (size_t)length;
    return ESP_OK;
}

esp_err_t httpd_resp_send_err(httpd_req_t *request, int status, const char *message)
{
    (void)request;
    (void)status;
    (void)message;
    return ESP_FAIL;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *uri)
{
    assert(server != NULL);
    assert(uri != NULL);
    ++register_calls;
    return register_result;
}

int64_t esp_timer_get_time(void)
{
    return 1000;
}

void taskYIELD(void)
{
    ++yield_calls;
}

const char *web_static_asset_data(web_static_asset_id_t asset)
{
    (void)asset;
    return "legacy";
}

#define WEB_SERVER_WEBUI_SEED_EMBEDDED 0
#include "../web_webui_flash.c"

static void reset_fake(const unsigned char *data, size_t length)
{
    memset(last_type, 0, sizeof(last_type));
    memset(header_fields, 0, sizeof(header_fields));
    memset(header_values, 0, sizeof(header_values));
    memset(sent_lengths, 0, sizeof(sent_lengths));
    type_calls = 0U;
    header_calls = 0U;
    body_send_calls = 0U;
    chunk_calls = 0U;
    final_chunk_calls = 0U;
    yield_calls = 0U;
    register_calls = 0U;
    fail_header_call = 0;
    fail_send_call = 0;
    register_result = ESP_OK;
    expected_start = data;
    payload_length = length;
    expected_offset = 0U;
}

static void assert_common_headers(bool gzip)
{
    assert(type_calls == 1U);
    assert(strcmp(last_type, "application/octet-stream") == 0);
    assert(header_calls == (gzip ? 3U : 2U));
    assert(strcmp(header_fields[0], "Cache-Control") == 0);
    assert(strcmp(header_values[0], "no-store") == 0);
    assert(strcmp(header_fields[1], "Connection") == 0);
    assert(strcmp(header_values[1], "close") == 0);
    if (gzip) {
        assert(strcmp(header_fields[2], "Content-Encoding") == 0);
        assert(strcmp(header_values[2], "gzip") == 0);
    }
}

static void test_chunk_boundaries(void)
{
    unsigned char data[4097];
    for (size_t i = 0; i < sizeof(data); ++i) {
        data[i] = (unsigned char)(i * 17U + 3U);
    }
    httpd_req_t request = {.uri = "/assets/app.js"};

    const size_t lengths[] = {0U, 1U, 2047U, 2048U, 2049U, 4096U, 4097U};
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        const size_t length = lengths[i];
        reset_fake(data, length);
        assert(web_webui_flash_send(
            &request,
            data,
            data + length,
            "application/octet-stream",
            false,
            false,
            "no-store"
        ) == ESP_OK);
        assert(expected_offset == length);
        assert_common_headers(false);
        if (length <= 2048U) {
            assert(body_send_calls == 1U);
            assert(chunk_calls == 0U);
            assert(yield_calls == 0U);
        }
        else {
            assert(body_send_calls == 0U);
            assert(chunk_calls == (length + 2047U) / 2048U + 1U);
            assert(final_chunk_calls == 1U);
            assert(yield_calls == (length + 2047U) / 2048U);
        }
    }
}

static void test_headers_and_send_errors_propagate(void)
{
    const unsigned char data[2049] = {0};
    httpd_req_t request = {.uri = "/assets/app.js"};

    reset_fake(data, sizeof(data));
    fail_header_call = 2;
    assert(web_webui_flash_send(
        &request, data, data + sizeof(data), "application/octet-stream",
        true, false, "no-store"
    ) == ESP_FAIL);
    assert(body_send_calls == 0U);
    assert(chunk_calls == 0U);

    reset_fake(data, sizeof(data));
    fail_send_call = 2;
    assert(web_webui_flash_send(
        &request, data, data + sizeof(data), "application/octet-stream",
        false, false, "no-store"
    ) == ESP_FAIL);
    assert(chunk_calls == 2U);
    assert(final_chunk_calls == 0U);
    assert(expected_offset == 2048U);
}

static void test_uri_and_registration_contracts(void)
{
    assert(web_webui_uri_equals("/assets/app.js", "/assets/app.js"));
    assert(web_webui_uri_equals("/assets/app.js?cache=1", "/assets/app.js"));
    assert(!web_webui_uri_equals("/assets/app.jsx", "/assets/app.js"));
    assert(!web_webui_uri_equals(NULL, "/assets/app.js"));
    assert(!web_webui_uri_equals("/assets/app.js", NULL));

    httpd_req_t request = {.uri = "/assets/dashboard.js?cache=1"};
    const unsigned char legacy[] = "legacy";
    reset_fake(legacy, sizeof(legacy) - 1U);
    assert(web_webui_flash_asset_handler(&request) == ESP_OK);
    assert(body_send_calls == 1U);
    assert(expected_offset == sizeof(legacy) - 1U);

    assert(web_webui_flash_register(NULL) == ESP_ERR_INVALID_ARG);
    reset_fake(legacy, 0U);
    assert(web_webui_flash_register((httpd_handle_t)&request) == ESP_OK);
    assert(register_calls == 1U);
    register_result = ESP_FAIL;
    assert(web_webui_flash_register((httpd_handle_t)&request) == ESP_FAIL);
}

int main(void)
{
    test_chunk_boundaries();
    test_headers_and_send_errors_propagate();
    test_uri_and_registration_contracts();
    return 0;
}
