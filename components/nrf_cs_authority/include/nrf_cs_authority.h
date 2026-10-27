#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nrf_cs_control_protocol.h"
#include "nrf_cs_scheduler.h"

typedef enum {
    NRF_CS_AUTHORITY_OK = 0,
    NRF_CS_AUTHORITY_PROGRESS = 1,
    NRF_CS_AUTHORITY_COMPLETE = 2,
    NRF_CS_AUTHORITY_ERR_ARGUMENT = -1,
    NRF_CS_AUTHORITY_ERR_BUSY = -2,
    NRF_CS_AUTHORITY_ERR_STALE = -3,
    NRF_CS_AUTHORITY_ERR_TRANSPORT = -4,
    NRF_CS_AUTHORITY_ERR_STATE = -5,
} nrf_cs_authority_result_t;

typedef struct {
    void *context;
    int (*write_execute)(void *context, const nrf_cs_control_request_t *request);
    int (*poll_result)(void *context, const nrf_cs_control_request_t *request, nrf_cs_control_result_t *result);
    uint64_t (*now_ms)(void *context);
} nrf_cs_authority_ops_t;

typedef struct {
    nrf_cs_scheduler_t scheduler;
    nrf_cs_authority_ops_t ops;
    uint32_t next_request_id;
    bool cycle_active;
    bool command_active;
    bool execute_acked;
    uint8_t execute_failures;
    uint64_t deadline_ms;
    nrf_cs_control_request_t request;
} nrf_cs_authority_t;

nrf_cs_authority_result_t nrf_cs_authority_init(nrf_cs_authority_t *authority, const nrf_cs_policy_t *policy, const nrf_cs_authority_ops_t *ops, uint32_t request_seed);
nrf_cs_authority_result_t nrf_cs_authority_start_cycle(nrf_cs_authority_t *authority, const nrf_cs_node_id_t *nodes, size_t node_count);
nrf_cs_authority_result_t nrf_cs_authority_pump(nrf_cs_authority_t *authority);
nrf_cs_authority_result_t nrf_cs_authority_abort_cycle(
    nrf_cs_authority_t *authority,
    nrf_cs_failure_reason_t reason);
/*
 * A cycle whose scheduler was completed from outside (autonomous ranging replays the nodes' records
 * through it): ends the cycle. ERR_STATE unless a cycle is active and its scheduler is complete.
 */
nrf_cs_authority_result_t nrf_cs_authority_finish_external(nrf_cs_authority_t *authority);
nrf_cs_authority_result_t nrf_cs_authority_disconnect(nrf_cs_authority_t *authority, nrf_cs_node_id_t node_id);
bool nrf_cs_authority_is_active(const nrf_cs_authority_t *authority);
size_t nrf_cs_authority_pair_count(const nrf_cs_authority_t *authority);
const nrf_cs_pair_result_t *nrf_cs_authority_pair_at(const nrf_cs_authority_t *authority, size_t pair_index);
