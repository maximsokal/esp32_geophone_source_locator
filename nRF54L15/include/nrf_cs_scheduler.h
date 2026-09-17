#ifndef NRF_CS_SCHEDULER_H
#define NRF_CS_SCHEDULER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NRF_CS_MAX_NODES 5U
#define NRF_CS_MAX_PAIRS 10U
#define NRF_CS_MAX_EXECUTIONS 10U
#define NRF_CS_CONFIDENCE_MAX 1000U

/*
 * Platform-neutral Channel Sounding cycle core. Bluetooth/NCS adapters feed
 * provider outcomes into this scheduler; the scheduler never owns a link.
 */
typedef uint8_t nrf_cs_node_id_t;

typedef enum {
    NRF_CS_OK = 0,
    NRF_CS_STEP_READY = 1,
    NRF_CS_CYCLE_COMPLETE = 2,
    NRF_CS_ERR_ARGUMENT = -1,
    NRF_CS_ERR_BUSY = -2,
    NRF_CS_ERR_STALE = -3,
    NRF_CS_ERR_NOT_FOUND = -4,
} nrf_cs_result_t;

typedef enum {
    NRF_CS_PAIR_UNKNOWN = 0,
    NRF_CS_PAIR_PENDING,
    NRF_CS_PAIR_VALID,
    NRF_CS_PAIR_MISSING,
    NRF_CS_PAIR_RETRY_EXHAUSTED,
} nrf_cs_pair_state_t;

typedef enum {
    NRF_CS_FAILURE_NONE = 0,
    NRF_CS_FAILURE_TIMEOUT,
    NRF_CS_FAILURE_TRANSPORT,
    NRF_CS_FAILURE_OUTLIER,
    NRF_CS_FAILURE_DISCONNECTED,
} nrf_cs_failure_reason_t;

typedef enum {
    NRF_CS_OUTCOME_SUCCESS = 0,
    NRF_CS_OUTCOME_TIMEOUT,
    NRF_CS_OUTCOME_TRANSPORT_ERROR,
    NRF_CS_OUTCOME_OUTLIER,
} nrf_cs_outcome_t;

typedef enum {
    NRF_CS_QUALITY_NONE = 0,
    NRF_CS_QUALITY_HAD_RETRY = 1U << 0,
    NRF_CS_QUALITY_LOW_CONFIDENCE = 1U << 1,
    NRF_CS_QUALITY_OUTLIER = 1U << 2,
    NRF_CS_QUALITY_TIMEOUT = 1U << 3,
    NRF_CS_QUALITY_TRANSPORT_ERROR = 1U << 4,
    NRF_CS_QUALITY_PARTIAL = 1U << 5,
} nrf_cs_scheduler_quality_t;

typedef struct {
    uint32_t pair_timeout_ms;
    uint8_t max_attempts;
    uint16_t min_confidence;
} nrf_cs_policy_t;

typedef struct {
    nrf_cs_outcome_t outcome;
    int32_t distance_mm;
    uint16_t confidence;
    uint16_t provider_quality_flags;
} nrf_cs_measurement_t;

typedef struct {
    nrf_cs_node_id_t node_a;
    nrf_cs_node_id_t node_b;
    nrf_cs_pair_state_t state;
    nrf_cs_failure_reason_t last_failure;
    int32_t distance_mm;
    uint16_t confidence;
    uint16_t provider_quality_flags;
    uint16_t scheduler_quality_flags;
    uint16_t attempts;
    uint8_t valid_samples;
} nrf_cs_pair_result_t;

/* A directed execution token for one undirected matrix cell. */
typedef struct {
    uint32_t cycle_id;
    uint32_t membership_generation;
    uint32_t attempt_id;
    uint8_t pair_index;
    uint8_t attempt_number;
    nrf_cs_node_id_t initiator;
    nrf_cs_node_id_t reflector;
    uint32_t timeout_ms;
} nrf_cs_step_t;

typedef struct {
    uint8_t pair_index;
    nrf_cs_node_id_t initiator;
    nrf_cs_node_id_t reflector;
    uint8_t attempts;
    bool terminal;
    bool succeeded;
    nrf_cs_failure_reason_t last_failure;
} nrf_cs_execution_t;

typedef struct {
    nrf_cs_policy_t policy;
    uint32_t cycle_id;
    uint32_t membership_generation;
    uint32_t cycle_membership_generation;
    uint32_t next_attempt_id;
    nrf_cs_node_id_t nodes[NRF_CS_MAX_NODES];
    size_t node_count;
    uint8_t disconnected_mask;
    nrf_cs_pair_result_t pairs[NRF_CS_MAX_PAIRS];
    int64_t distance_sum_mm[NRF_CS_MAX_PAIRS];
    uint32_t confidence_sum[NRF_CS_MAX_PAIRS];
    size_t pair_count;
    nrf_cs_execution_t executions[NRF_CS_MAX_EXECUTIONS];
    size_t execution_count;
    bool cycle_started;
    bool active;
    size_t active_execution_index;
    nrf_cs_step_t active_step;
} nrf_cs_scheduler_t;

nrf_cs_result_t nrf_cs_scheduler_init(
    nrf_cs_scheduler_t *scheduler,
    const nrf_cs_policy_t *policy);

nrf_cs_result_t nrf_cs_scheduler_start_cycle(
    nrf_cs_scheduler_t *scheduler,
    const nrf_cs_node_id_t *node_ids,
    size_t node_count);

nrf_cs_result_t nrf_cs_scheduler_next_step(
    nrf_cs_scheduler_t *scheduler,
    nrf_cs_step_t *step_out);

nrf_cs_result_t nrf_cs_scheduler_complete_step(
    nrf_cs_scheduler_t *scheduler,
    const nrf_cs_step_t *step,
    const nrf_cs_measurement_t *measurement);

nrf_cs_result_t nrf_cs_scheduler_disconnect(
    nrf_cs_scheduler_t *scheduler,
    nrf_cs_node_id_t node_id);

bool nrf_cs_scheduler_is_complete(const nrf_cs_scheduler_t *scheduler);
size_t nrf_cs_scheduler_pair_count(const nrf_cs_scheduler_t *scheduler);
const nrf_cs_pair_result_t *nrf_cs_scheduler_pair_at(
    const nrf_cs_scheduler_t *scheduler,
    size_t pair_index);
const nrf_cs_pair_result_t *nrf_cs_scheduler_matrix_cell(
    const nrf_cs_scheduler_t *scheduler,
    nrf_cs_node_id_t node_a,
    nrf_cs_node_id_t node_b);

#ifdef __cplusplus
}
#endif

#endif
