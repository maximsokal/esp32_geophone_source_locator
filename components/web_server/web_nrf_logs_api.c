#include "web_nrf_logs_api.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "nrf_topology_service.h"

esp_err_t web_server_route_tracking_register(const char *uri, int method);

#define WEB_NRF_LOGS_QUERY_MAX 128U
#define WEB_NRF_LOGS_JSON_RECORD_BUDGET 256U
#define WEB_NRF_LOGS_DEFAULT_LIMIT 50U

typedef struct {
    const char *value;
    size_t length;
    bool found;
} web_nrf_logs_value_t;

static web_nrf_logs_value_t query_value(const char *query, size_t length, const char *key)
{
    web_nrf_logs_value_t result = {0};
    const size_t key_length = strlen(key);
    size_t start = 0U;
    while (start < length) {
        size_t end = start;
        size_t equals = SIZE_MAX;
        while (end < length && query[end] != '&') {
            if (query[end] == '=' && equals == SIZE_MAX) equals = end;
            ++end;
        }
        if (equals != SIZE_MAX && equals - start == key_length &&
            memcmp(query + start, key, key_length) == 0) {
            result.found = true;
            result.value = query + equals + 1U;
            result.length = end - equals - 1U;
            return result;
        }
        start = end + 1U;
    }
    return result;
}

static bool parse_node(const char *value, size_t length, uint8_t *node_index)
{
    if (length == 4U && memcmp(value, "main", 4U) == 0) {
        *node_index = 0U;
        return true;
    }
    if (length == 7U && memcmp(value, "remote", 6U) == 0 &&
        value[6] >= '0' && value[6] <= '7') {
        *node_index = (uint8_t)(value[6] - '0' + 1);
        return true;
    }
    return false;
}

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static bool parse_token(const char *value, size_t length, uint32_t *boot_id, uint32_t *seq)
{
    uint32_t parts[2] = {0U, 0U};
    if (length != 17U || value[8] != '-') return false;
    for (size_t i = 0U; i < 17U; ++i) {
        if (i == 8U) continue;
        const int nibble = hex_value(value[i]);
        if (nibble < 0) return false;
        const size_t part = i < 8U ? 0U : 1U;
        parts[part] = (parts[part] << 4U) | (uint32_t)nibble;
    }
    *boot_id = parts[0];
    *seq = parts[1];
    return true;
}

bool web_nrf_logs_parse_query(
    const char *query,
    size_t query_length,
    web_nrf_logs_query_t *parsed,
    web_nrf_logs_bad_query_t *reason)
{
    if (query == NULL || parsed == NULL || reason == NULL) return false;
    const web_nrf_logs_value_t node = query_value(query, query_length, "node");
    const web_nrf_logs_value_t limit = query_value(query, query_length, "limit");
    const web_nrf_logs_value_t after = query_value(query, query_length, "after");
    memset(parsed, 0, sizeof(*parsed));
    parsed->limit = WEB_NRF_LOGS_DEFAULT_LIMIT;
    if (!node.found || !parse_node(node.value, node.length, &parsed->node_index)) {
        *reason = WEB_NRF_LOGS_BAD_NODE;
        return false;
    }
    if (limit.found) {
        uint32_t value = 0U;
        if (limit.length == 0U) {
            *reason = WEB_NRF_LOGS_BAD_LIMIT;
            return false;
        }
        for (size_t i = 0U; i < limit.length; ++i) {
            if (limit.value[i] < '0' || limit.value[i] > '9') {
                *reason = WEB_NRF_LOGS_BAD_LIMIT;
                return false;
            }
            const uint32_t digit = (uint32_t)(limit.value[i] - '0');
            if (value > (NRF_LOG_COLLECTOR_PAGE_MAX - digit) / 10U) {
                *reason = WEB_NRF_LOGS_BAD_LIMIT;
                return false;
            }
            value = value * 10U + digit;
        }
        if (value == 0U) {
            *reason = WEB_NRF_LOGS_BAD_LIMIT;
            return false;
        }
        parsed->limit = (uint16_t)value;
    }
    if (after.found) {
        if (!parse_token(after.value, after.length, &parsed->after_boot_id, &parsed->after_seq)) {
            *reason = WEB_NRF_LOGS_BAD_TOKEN;
            return false;
        }
        parsed->has_after = true;
    }
    return true;
}

const char *web_nrf_logs_bad_query_name(web_nrf_logs_bad_query_t reason)
{
    switch (reason) {
        case WEB_NRF_LOGS_BAD_NODE: return "bad_node";
        case WEB_NRF_LOGS_BAD_LIMIT: return "bad_limit";
        case WEB_NRF_LOGS_BAD_TOKEN: return "bad_token";
        default: return "bad_node";
    }
}

static bool append_format(char *buffer, size_t capacity, size_t *used, const char *format, ...)
{
    int written;
    va_list args;
    if (*used >= capacity) return false;
    va_start(args, format);
    written = vsnprintf(buffer + *used, capacity - *used, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= capacity - *used) return false;
    *used += (size_t)written;
    return true;
}

static void token_format(uint32_t boot_id, uint32_t seq, char token[18])
{
    (void)snprintf(token, 18U, "%08" PRIx32 "-%08" PRIx32, boot_id, seq);
}

esp_err_t web_nrf_logs_format_json(
    uint8_t node_index,
    const nrf_log_node_summary_t *summary,
    const nrf_log_node_page_t *page,
    char *buffer,
    size_t capacity,
    size_t *length_out)
{
    size_t used = 0U;
    char remote_name[8];
    const char *node_name;
    if (node_index >= NRF_LOG_COLLECTOR_NODE_COUNT || summary == NULL || page == NULL ||
        buffer == NULL || length_out == NULL || page->count > NRF_LOG_COLLECTOR_RECORD_CAPACITY) {
        return ESP_ERR_INVALID_ARG;
    }
    node_name = node_index == 0U ? "main" : remote_name;
    if (node_index != 0U) {
        (void)snprintf(remote_name, sizeof(remote_name), "remote%u", (unsigned)(node_index - 1U));
    }
    if (!append_format(buffer, capacity, &used,
            "{\"ok\":true,\"node\":\"%s\",\"channel\":", node_name)) return ESP_ERR_NO_MEM;
    if (!(node_index == 0U
            ? append_format(buffer, capacity, &used, "null")
            : append_format(buffer, capacity, &used, "%u", (unsigned)(node_index - 1U)))) {
        return ESP_ERR_NO_MEM;
    }
    if (!append_format(buffer, capacity, &used, ",\"records\":[")) return ESP_ERR_NO_MEM;
    for (uint16_t i = 0U; i < page->count; ++i) {
        const nrf_log_stored_record_t *record = &page->records[i];
        char token[18];
        token_format(record->boot_id, record->seq, token);
        if (!append_format(buffer, capacity, &used,
                "%s{\"token\":\"%s\",\"boot_id\":%" PRIu32
                ",\"seq\":%" PRIu32 ",\"uptime_ms\":%" PRIu32
                ",\"esp_rx_uptime_ms\":%" PRIu32 ",\"event_id\":%u"
                ",\"severity\":%u,\"subsystem\":%u,\"arg0\":%" PRIu32
                ",\"arg1\":%" PRIu32 "}",
                i == 0U ? "" : ",",
                token,
                record->boot_id,
                record->seq,
                record->uptime_ms,
                record->esp_rx_uptime_ms,
                (unsigned)record->event_id,
                (unsigned)record->severity,
                (unsigned)record->subsystem,
                record->arg0,
                record->arg1)) return ESP_ERR_NO_MEM;
    }
    if (!append_format(buffer, capacity, &used, "],\"next_token\":")) return ESP_ERR_NO_MEM;
    if (page->next_token_valid) {
        char token[18];
        token_format(page->next_token_boot_id, page->next_token_seq, token);
        if (!append_format(buffer, capacity, &used, "\"%s\"", token)) return ESP_ERR_NO_MEM;
    } else if (!append_format(buffer, capacity, &used, "null")) {
        return ESP_ERR_NO_MEM;
    }
    if (!append_format(buffer, capacity, &used,
            ",\"reset\":%s,\"counters\":{\"overwritten_total\":%" PRIu32
            ",\"lost_internal_total\":%" PRIu32 ",\"wire_gap_total\":%" PRIu32
            ",\"local_drop_total\":%" PRIu32 "},\"logs_unsupported\":%s,\"boot_id\":",
            page->reset ? "true" : "false",
            summary->overwritten_total,
            summary->lost_internal_total,
            summary->wire_gap_total,
            summary->local_drop_total,
            summary->logs_unsupported ? "true" : "false")) return ESP_ERR_NO_MEM;
    if (!(summary->boot_id_valid
            ? append_format(buffer, capacity, &used, "%" PRIu32, summary->boot_id)
            : append_format(buffer, capacity, &used, "null"))) return ESP_ERR_NO_MEM;
    if (!append_format(buffer, capacity, &used, "}")) return ESP_ERR_NO_MEM;
    *length_out = used;
    return ESP_OK;
}

static esp_err_t send_json_error(httpd_req_t *request, const char *status, const char *reason)
{
    char body[64];
    (void)snprintf(body, sizeof(body), "{\"ok\":false,\"reason\":\"%s\"}", reason);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_status(request, status);
    return httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_bad_query(httpd_req_t *request, web_nrf_logs_bad_query_t reason)
{
    char body[64];
    (void)snprintf(body, sizeof(body), "{\"ok\":false,\"reason\":\"%s\"}",
                   web_nrf_logs_bad_query_name(reason));
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_status(request, "400 Bad Request");
    return httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t web_nrf_logs_handler(httpd_req_t *request)
{
    char query_string[WEB_NRF_LOGS_QUERY_MAX];
    web_nrf_logs_query_t query;
    web_nrf_logs_bad_query_t reason = WEB_NRF_LOGS_BAD_NODE;
    nrf_log_node_page_t *page = NULL;
    char *json = NULL;
    size_t json_length = 0U;
    const size_t query_length = httpd_req_get_url_query_len(request);
    if (query_length >= sizeof(query_string)) {
        return send_bad_query(request, WEB_NRF_LOGS_BAD_TOKEN);
    }
    if (query_length != 0U &&
        httpd_req_get_url_query_str(request, query_string, sizeof(query_string)) != ESP_OK) {
        return send_bad_query(request, WEB_NRF_LOGS_BAD_NODE);
    }
    if (query_length == 0U) query_string[0] = '\0';
    if (!web_nrf_logs_parse_query(query_string, query_length, &query, &reason)) {
        return send_bad_query(request, reason);
    }

    page = heap_caps_calloc(1U, sizeof(*page), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (page == NULL) return send_json_error(request, "500 Internal Server Error", "internal_error");
    if (nrf_topology_service_get_nrf_log_page(
            query.node_index,
            query.has_after,
            query.after_boot_id,
            query.after_seq,
            query.limit,
            page) != ESP_OK) {
        heap_caps_free(page);
        return send_json_error(request, "503 Service Unavailable", "temporarily_unavailable");
    }

    const size_t capacity = 512U + (size_t)page->count * WEB_NRF_LOGS_JSON_RECORD_BUDGET;
    json = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (json == NULL) {
        heap_caps_free(page);
        return send_json_error(request, "500 Internal Server Error", "internal_error");
    }
    const esp_err_t format_err = web_nrf_logs_format_json(
        query.node_index,
        &page->summary,
        page,
        json,
        capacity,
        &json_length);
    if (format_err != ESP_OK) {
        heap_caps_free(json);
        heap_caps_free(page);
        return send_json_error(request, "500 Internal Server Error", "internal_error");
    }

    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    const esp_err_t send_err = httpd_resp_send(request, json, (int)json_length);
    heap_caps_free(json);
    heap_caps_free(page);
    return send_err;
}

esp_err_t web_nrf_logs_api_register(httpd_handle_t server)
{
    if (server == NULL) return ESP_ERR_INVALID_ARG;
    const httpd_uri_t route = {
        .uri = "/api/nrf/logs",
        .method = HTTP_GET,
        .handler = web_nrf_logs_handler,
        .user_ctx = NULL,
    };
    const esp_err_t err = httpd_register_uri_handler(server, &route);
    if (err != ESP_OK) return err;
    return web_server_route_tracking_register(route.uri, route.method);
}
