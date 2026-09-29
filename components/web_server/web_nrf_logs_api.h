#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_http_server.h"
#include "nrf_log_collector.h"

typedef enum {
    WEB_NRF_LOGS_BAD_NODE = 0,
    WEB_NRF_LOGS_BAD_LIMIT,
    WEB_NRF_LOGS_BAD_TOKEN,
} web_nrf_logs_bad_query_t;

typedef struct {
    uint8_t node_index;
    uint16_t limit;
    uint32_t after_boot_id;
    uint32_t after_seq;
    bool has_after;
} web_nrf_logs_query_t;

bool web_nrf_logs_parse_query(
    const char *query,
    size_t query_length,
    web_nrf_logs_query_t *parsed,
    web_nrf_logs_bad_query_t *reason);
esp_err_t web_nrf_logs_format_json(
    uint8_t node_index,
    const nrf_log_node_summary_t *summary,
    const nrf_log_node_page_t *page,
    char *buffer,
    size_t capacity,
    size_t *length_out);
const char *web_nrf_logs_bad_query_name(web_nrf_logs_bad_query_t reason);
esp_err_t web_nrf_logs_api_register(httpd_handle_t server);
