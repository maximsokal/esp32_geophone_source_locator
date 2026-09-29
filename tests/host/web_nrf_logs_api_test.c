#include "web_nrf_logs_api.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"

static httpd_uri_t s_route;
static nrf_log_node_page_t s_page;
static uint8_t s_seen_node;
static uint16_t s_seen_limit;
static bool s_seen_after;

size_t httpd_req_get_url_query_len(httpd_req_t *request)
{
    return request->query_length;
}

esp_err_t httpd_req_get_url_query_str(httpd_req_t *request, char *buffer, size_t buffer_size)
{
    if (request->query_length + 1U > buffer_size) return ESP_ERR_INVALID_ARG;
    memcpy(buffer, request->query, request->query_length);
    buffer[request->query_length] = '\0';
    return ESP_OK;
}

esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type)
{
    (void)request;
    (void)type;
    return ESP_OK;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *field, const char *value)
{
    (void)request;
    (void)field;
    (void)value;
    return ESP_OK;
}

esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status)
{
    (void)snprintf(request->status, sizeof(request->status), "%s", status);
    return ESP_OK;
}

esp_err_t httpd_resp_send(httpd_req_t *request, const char *buffer, int length)
{
    const size_t actual = length == HTTPD_RESP_USE_STRLEN ? strlen(buffer) : (size_t)length;
    assert(actual < sizeof(request->response));
    memcpy(request->response, buffer, actual);
    request->response[actual] = '\0';
    request->response_length = actual;
    return ESP_OK;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *route)
{
    (void)server;
    s_route = *route;
    return ESP_OK;
}

esp_err_t web_server_route_tracking_register(const char *uri, int method)
{
    assert(strcmp(uri, "/api/nrf/logs") == 0);
    assert(method == HTTP_GET);
    return ESP_OK;
}

esp_err_t nrf_topology_service_get_nrf_log_page(
    uint8_t node_index,
    bool has_after,
    uint32_t after_boot_id,
    uint32_t after_seq,
    uint16_t limit,
    nrf_log_node_page_t *page)
{
    s_seen_node = node_index;
    s_seen_limit = limit;
    s_seen_after = has_after;
    if (has_after) {
        assert(after_boot_id == UINT32_C(0xABCD1234));
        assert(after_seq == 9U);
    }
    *page = s_page;
    if (page->count > limit) page->count = limit;
    if (page->count != 0U) {
        page->next_token_valid = true;
        page->next_token_boot_id = page->records[page->count - 1U].boot_id;
        page->next_token_seq = page->records[page->count - 1U].seq;
    }
    return ESP_OK;
}

static void test_query_validation(void)
{
    web_nrf_logs_query_t parsed;
    web_nrf_logs_bad_query_t reason;

    assert(!web_nrf_logs_parse_query("limit=1", 7U, &parsed, &reason));
    assert(reason == WEB_NRF_LOGS_BAD_NODE);
    assert(!web_nrf_logs_parse_query("node=remote8", 12U, &parsed, &reason));
    assert(reason == WEB_NRF_LOGS_BAD_NODE);
    assert(!web_nrf_logs_parse_query("node=main&limit=0", 16U, &parsed, &reason));
    assert(reason == WEB_NRF_LOGS_BAD_LIMIT);
    assert(!web_nrf_logs_parse_query("node=main&limit=201", 19U, &parsed, &reason));
    assert(reason == WEB_NRF_LOGS_BAD_LIMIT);
    assert(!web_nrf_logs_parse_query("node=main&limit=999999999999999999999", 38U, &parsed, &reason));
    assert(reason == WEB_NRF_LOGS_BAD_LIMIT);
    assert(!web_nrf_logs_parse_query("node=main&after=not-a-token", 26U, &parsed, &reason));
    assert(reason == WEB_NRF_LOGS_BAD_TOKEN);

    assert(web_nrf_logs_parse_query(
        "node=remote2&after=ABCD1234-00000009&limit=200",
        46U,
        &parsed,
        &reason));
    assert(parsed.node_index == 3U);
    assert(parsed.limit == 200U);
    assert(parsed.has_after);
    assert(parsed.after_boot_id == UINT32_C(0xABCD1234));
    assert(parsed.after_seq == 9U);
    assert(web_nrf_logs_parse_query("node=main", 9U, &parsed, &reason));
    assert(parsed.limit == 50U);
    assert(!parsed.has_after);
}

static void test_http_golden_and_bad_requests(void)
{
    httpd_req_t request = {0};
    const char *expected =
        "{\"ok\":true,\"node\":\"remote2\",\"channel\":2,\"records\":["
        "{\"token\":\"000012ab-00000034\",\"boot_id\":4779,\"seq\":52,"
        "\"uptime_ms\":56,\"esp_rx_uptime_ms\":78,\"event_id\":90,"
        "\"severity\":3,\"subsystem\":5,\"arg0\":305419896,\"arg1\":4294967295}],"
        "\"next_token\":\"000012ab-00000034\",\"reset\":false,"
        "\"counters\":{\"overwritten_total\":7,\"lost_internal_total\":8,"
        "\"wire_gap_total\":9,\"local_drop_total\":10},"
        "\"logs_unsupported\":true,\"boot_id\":4779}";

    memset(&s_page, 0, sizeof(s_page));
    s_page.count = 1U;
    s_page.next_token_valid = true;
    s_page.next_token_boot_id = UINT32_C(0x12AB);
    s_page.next_token_seq = 52U;
    s_page.records[0] = (nrf_log_stored_record_t) {
        .boot_id = UINT32_C(0x12AB),
        .seq = 52U,
        .uptime_ms = 56U,
        .esp_rx_uptime_ms = 78U,
        .event_id = 90U,
        .severity = 3U,
        .subsystem = 5U,
        .arg0 = UINT32_C(0x12345678),
        .arg1 = UINT32_MAX,
    };
    s_page.summary = (nrf_log_node_summary_t) {
        .present = true,
        .logs_unsupported = true,
        .boot_id_valid = true,
        .boot_id = UINT32_C(0x12AB),
        .stored = 1U,
        .warn_count = 0U,
        .error_count = 1U,
        .fatal_count = 0U,
        .overwritten_total = 7U,
        .lost_internal_total = 8U,
        .wire_gap_total = 9U,
        .local_drop_total = 10U,
    };

    assert(web_nrf_logs_api_register(&request) == ESP_OK);
    assert(strcmp(s_route.uri, "/api/nrf/logs") == 0);
    assert(s_route.method == HTTP_GET);

    request.query = "node=remote2&after=ABCD1234-00000009&limit=1";
    request.query_length = strlen(request.query);
    assert(s_route.handler(&request) == ESP_OK);
    assert(strcmp(request.response, expected) == 0);
    assert(s_seen_node == 3U && s_seen_limit == 1U && s_seen_after);

    memset(&request, 0, sizeof(request));
    request.query = "node=remote8";
    request.query_length = strlen(request.query);
    assert(s_route.handler(&request) == ESP_OK);
    assert(strcmp(request.status, "400 Bad Request") == 0);
    assert(strcmp(request.response, "{\"ok\":false,\"reason\":\"bad_node\"}") == 0);

    memset(&request, 0, sizeof(request));
    request.query = "node=main&limit=0";
    request.query_length = strlen(request.query);
    assert(s_route.handler(&request) == ESP_OK);
    assert(strcmp(request.response, "{\"ok\":false,\"reason\":\"bad_limit\"}") == 0);

    memset(&request, 0, sizeof(request));
    request.query = "node=main&after=bad";
    request.query_length = strlen(request.query);
    assert(s_route.handler(&request) == ESP_OK);
    assert(strcmp(request.response, "{\"ok\":false,\"reason\":\"bad_token\"}") == 0);
}

int main(void)
{
    test_query_validation();
    test_http_golden_and_bad_requests();
    puts("Web nRF logs API tests passed");
    return 0;
}
