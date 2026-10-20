#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nrf_topology.h"

#define NRF_LOG_COLLECTOR_NODE_COUNT (NRF_TOPOLOGY_REMOTE_COUNT + 1U)
#define NRF_LOG_COLLECTOR_RECORD_CAPACITY 128U
#define NRF_LOG_COLLECTOR_PAGE_MAX 200U
/* ESP-only reboot marker: event_id 0xFFFF, seq UINT32_MAX, arg0/arg1 old/new boot id. */
#define NRF_LOG_COLLECTOR_REBOOT_EVENT_ID UINT16_C(0xFFFF)

typedef struct {
    uint32_t boot_id;
    uint32_t seq;
    uint32_t uptime_ms;
    uint32_t esp_rx_uptime_ms;
    uint16_t event_id;
    uint8_t severity;
    uint8_t subsystem;
    uint32_t arg0;
    uint32_t arg1;
} nrf_log_stored_record_t;

typedef struct {
    bool present;
    bool logs_unsupported;
    bool boot_id_valid;
    uint32_t boot_id;
    uint16_t stored;
    uint16_t warn_count;
    uint16_t error_count;
    uint16_t fatal_count;
    uint32_t overwritten_total;
    uint32_t lost_internal_total;
    uint32_t wire_gap_total;
    uint32_t local_drop_total;
} nrf_log_node_summary_t;

typedef struct {
    nrf_log_stored_record_t records[NRF_LOG_COLLECTOR_RECORD_CAPACITY];
    nrf_log_node_summary_t summary;
    uint16_t count;
    bool reset;
    bool next_token_valid;
    uint32_t next_token_boot_id;
    uint32_t next_token_seq;
} nrf_log_node_page_t;

typedef struct {
    bool present;
    bool route_available;
    bool link_ready;
    bool cs_busy;
    uint8_t node_id;
    uint8_t mux_channel;
    uint8_t current_address;
    uint8_t hardware_id_length;
    uint8_t hardware_id[NRF_UPSTREAM_PROTOCOL_HW_ID_MAX];
    uint32_t assignment_generation;
} nrf_log_poll_target_t;

typedef struct {
    void *context;
    esp_err_t (*begin_node)(void *context, uint8_t node_index);
    esp_err_t (*end_node)(void *context, uint8_t node_index);
    esp_err_t (*write_frame)(
        void *context,
        uint8_t node_index,
        const uint8_t *frame,
        size_t frame_length);
    esp_err_t (*read_frame)(
        void *context,
        uint8_t node_index,
        uint8_t *frame,
        size_t frame_capacity,
        size_t *received_length);
    uint64_t (*now_ms)(void *context);
    bool (*yield_requested)(void *context);
    bool (*node_link_ready)(void *context, uint8_t node_index);
    void (*report_link_failure)(void *context, uint8_t node_index);
} nrf_log_collector_io_t;

typedef struct {
    uint8_t nodes_attempted;
    uint8_t nodes_succeeded;
    uint8_t nodes_failed;
    uint8_t nodes_skipped_cs;
    uint32_t elapsed_ms;
} nrf_log_collector_iteration_t;

typedef struct nrf_log_collector nrf_log_collector_t;

nrf_log_collector_t *nrf_log_collector_create(void);
void nrf_log_collector_destroy(nrf_log_collector_t *collector);
esp_err_t nrf_log_collector_reset(nrf_log_collector_t *collector);
esp_err_t nrf_log_collector_sync_targets(
    nrf_log_collector_t *collector,
    const nrf_log_poll_target_t targets[NRF_LOG_COLLECTOR_NODE_COUNT]);
esp_err_t nrf_log_collector_poll_iteration(
    nrf_log_collector_t *collector,
    const nrf_log_poll_target_t targets[NRF_LOG_COLLECTOR_NODE_COUNT],
    const nrf_log_collector_io_t *io,
    uint32_t per_node_budget_ms,
    uint32_t total_budget_ms,
    nrf_log_collector_iteration_t *iteration);
esp_err_t nrf_log_collector_get_summary(
    nrf_log_collector_t *collector,
    uint8_t node_index,
    nrf_log_node_summary_t *summary);
esp_err_t nrf_log_collector_get_page(
    nrf_log_collector_t *collector,
    uint8_t node_index,
    bool has_after,
    uint32_t after_boot_id,
    uint32_t after_seq,
    uint16_t limit,
    nrf_log_node_page_t *page);
