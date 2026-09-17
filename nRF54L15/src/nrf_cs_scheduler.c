#include "nrf_cs_scheduler.h"

#include <string.h>

static bool nrf_cs_policy_valid(const nrf_cs_policy_t *policy)
{
    return policy != NULL &&
           policy->pair_timeout_ms > 0U &&
           policy->max_attempts > 0U &&
           policy->min_confidence <= NRF_CS_CONFIDENCE_MAX;
}

static void nrf_cs_sort_nodes(nrf_cs_node_id_t *nodes, size_t count)
{
    size_t i;

    for (i = 1U; i < count; ++i) {
        nrf_cs_node_id_t value = nodes[i];
        size_t j = i;

        while (j > 0U && nodes[j - 1U] > value) {
            nodes[j] = nodes[j - 1U];
            --j;
        }
        nodes[j] = value;
    }
}

static bool nrf_cs_nodes_unique(const nrf_cs_node_id_t *nodes, size_t count)
{
    size_t i;

    for (i = 1U; i < count; ++i) {
        if (nodes[i - 1U] == nodes[i]) {
            return false;
        }
    }
    return true;
}

static bool nrf_cs_membership_matches(
    const nrf_cs_scheduler_t *scheduler,
    const nrf_cs_node_id_t *nodes,
    size_t count)
{
    return scheduler->cycle_started &&
           scheduler->node_count == count &&
           memcmp(scheduler->nodes, nodes, count * sizeof(nodes[0])) == 0;
}

static uint32_t nrf_cs_next_nonzero(uint32_t value)
{
    ++value;
    return value == 0U ? 1U : value;
}

static void nrf_cs_pair_init(
    nrf_cs_pair_result_t *pair,
    nrf_cs_node_id_t node_a,
    nrf_cs_node_id_t node_b)
{
    memset(pair, 0, sizeof(*pair));
    pair->node_a = node_a;
    pair->node_b = node_b;
    pair->state = NRF_CS_PAIR_PENDING;
}

static void nrf_cs_add_execution(
    nrf_cs_scheduler_t *scheduler,
    uint8_t pair_index,
    nrf_cs_node_id_t initiator,
    nrf_cs_node_id_t reflector)
{
    nrf_cs_execution_t *execution = &scheduler->executions[scheduler->execution_count++];

    memset(execution, 0, sizeof(*execution));
    execution->pair_index = pair_index;
    execution->initiator = initiator;
    execution->reflector = reflector;
}

static void nrf_cs_add_pair(
    nrf_cs_scheduler_t *scheduler,
    size_t left_index,
    size_t right_index)
{
    size_t pair_index = scheduler->pair_count++;
    nrf_cs_node_id_t left = scheduler->nodes[left_index];
    nrf_cs_node_id_t right = scheduler->nodes[right_index];

    nrf_cs_pair_init(&scheduler->pairs[pair_index], left, right);

    if (scheduler->node_count == 2U) {
        nrf_cs_add_execution(scheduler, (uint8_t)pair_index, left, right);
        nrf_cs_add_execution(scheduler, (uint8_t)pair_index, right, left);
        return;
    }

    {
        size_t delta = right_index - left_index;
        size_t half = scheduler->node_count / 2U;
        bool left_initiates = delta <= half;

        nrf_cs_add_execution(
            scheduler,
            (uint8_t)pair_index,
            left_initiates ? left : right,
            left_initiates ? right : left);
    }
}

static bool nrf_cs_execution_matches_step(
    const nrf_cs_scheduler_t *scheduler,
    const nrf_cs_step_t *step)
{
    return scheduler->active &&
           step->cycle_id == scheduler->active_step.cycle_id &&
           step->membership_generation == scheduler->active_step.membership_generation &&
           step->attempt_id == scheduler->active_step.attempt_id &&
           step->pair_index == scheduler->active_step.pair_index &&
           step->attempt_number == scheduler->active_step.attempt_number &&
           step->initiator == scheduler->active_step.initiator &&
           step->reflector == scheduler->active_step.reflector;
}

static nrf_cs_failure_reason_t nrf_cs_failure_from_outcome(nrf_cs_outcome_t outcome)
{
    switch (outcome) {
    case NRF_CS_OUTCOME_TIMEOUT:
        return NRF_CS_FAILURE_TIMEOUT;
    case NRF_CS_OUTCOME_TRANSPORT_ERROR:
        return NRF_CS_FAILURE_TRANSPORT;
    case NRF_CS_OUTCOME_OUTLIER:
        return NRF_CS_FAILURE_OUTLIER;
    case NRF_CS_OUTCOME_SUCCESS:
    default:
        return NRF_CS_FAILURE_NONE;
    }
}

static void nrf_cs_record_failure_quality(
    nrf_cs_pair_result_t *pair,
    nrf_cs_failure_reason_t failure)
{
    switch (failure) {
    case NRF_CS_FAILURE_TIMEOUT:
        pair->scheduler_quality_flags |= NRF_CS_QUALITY_TIMEOUT;
        break;
    case NRF_CS_FAILURE_TRANSPORT:
        pair->scheduler_quality_flags |= NRF_CS_QUALITY_TRANSPORT_ERROR;
        break;
    case NRF_CS_FAILURE_OUTLIER:
        pair->scheduler_quality_flags |= NRF_CS_QUALITY_OUTLIER;
        break;
    case NRF_CS_FAILURE_DISCONNECTED:
    case NRF_CS_FAILURE_NONE:
    default:
        break;
    }
}

static void nrf_cs_finalize_pair(nrf_cs_scheduler_t *scheduler, size_t pair_index)
{
    nrf_cs_pair_result_t *pair = &scheduler->pairs[pair_index];
    bool any_failure = false;
    bool any_disconnect = false;
    size_t i;

    for (i = 0U; i < scheduler->execution_count; ++i) {
        const nrf_cs_execution_t *execution = &scheduler->executions[i];

        if (execution->pair_index != pair_index) {
            continue;
        }
        if (!execution->terminal) {
            return;
        }
        if (!execution->succeeded) {
            any_failure = true;
            if (execution->last_failure == NRF_CS_FAILURE_DISCONNECTED) {
                any_disconnect = true;
            }
        }
    }

    if (pair->valid_samples > 0U) {
        pair->state = NRF_CS_PAIR_VALID;
        if (any_failure) {
            pair->scheduler_quality_flags |= NRF_CS_QUALITY_PARTIAL;
        }
        pair->distance_mm = (int32_t)(
            scheduler->distance_sum_mm[pair_index] / pair->valid_samples);
        pair->confidence = (uint16_t)(
            scheduler->confidence_sum[pair_index] / pair->valid_samples);
    } else if (any_disconnect) {
        pair->state = NRF_CS_PAIR_MISSING;
    } else {
        pair->state = NRF_CS_PAIR_RETRY_EXHAUSTED;
    }
}

static bool nrf_cs_execution_uses_node(
    const nrf_cs_execution_t *execution,
    nrf_cs_node_id_t node_id)
{
    return execution->initiator == node_id || execution->reflector == node_id;
}

nrf_cs_result_t nrf_cs_scheduler_init(
    nrf_cs_scheduler_t *scheduler,
    const nrf_cs_policy_t *policy)
{
    if (scheduler == NULL || !nrf_cs_policy_valid(policy)) {
        return NRF_CS_ERR_ARGUMENT;
    }

    memset(scheduler, 0, sizeof(*scheduler));
    scheduler->policy = *policy;
    return NRF_CS_OK;
}

nrf_cs_result_t nrf_cs_scheduler_start_cycle(
    nrf_cs_scheduler_t *scheduler,
    const nrf_cs_node_id_t *node_ids,
    size_t node_count)
{
    nrf_cs_node_id_t sorted[NRF_CS_MAX_NODES];
    bool membership_changed;
    size_t i;
    size_t j;

    if (scheduler == NULL ||
        node_count > NRF_CS_MAX_NODES ||
        (node_count > 0U && node_ids == NULL)) {
        return NRF_CS_ERR_ARGUMENT;
    }
    if (scheduler->active ||
        (scheduler->cycle_started && !nrf_cs_scheduler_is_complete(scheduler))) {
        return NRF_CS_ERR_BUSY;
    }

    if (node_count > 0U) {
        memcpy(sorted, node_ids, node_count * sizeof(sorted[0]));
        nrf_cs_sort_nodes(sorted, node_count);
        if (!nrf_cs_nodes_unique(sorted, node_count)) {
            return NRF_CS_ERR_ARGUMENT;
        }
    }

    membership_changed = !nrf_cs_membership_matches(scheduler, sorted, node_count) ||
                         scheduler->disconnected_mask != 0U;
    if (membership_changed) {
        scheduler->membership_generation =
            nrf_cs_next_nonzero(scheduler->membership_generation);
    }

    scheduler->cycle_id = nrf_cs_next_nonzero(scheduler->cycle_id);
    scheduler->cycle_membership_generation = scheduler->membership_generation;
    scheduler->node_count = node_count;
    if (node_count > 0U) {
        memcpy(scheduler->nodes, sorted, node_count * sizeof(sorted[0]));
    }
    if (node_count < NRF_CS_MAX_NODES) {
        memset(&scheduler->nodes[node_count], 0,
               (NRF_CS_MAX_NODES - node_count) * sizeof(scheduler->nodes[0]));
    }
    scheduler->disconnected_mask = 0U;
    scheduler->pair_count = 0U;
    scheduler->execution_count = 0U;
    scheduler->active = false;
    memset(scheduler->pairs, 0, sizeof(scheduler->pairs));
    memset(scheduler->distance_sum_mm, 0, sizeof(scheduler->distance_sum_mm));
    memset(scheduler->confidence_sum, 0, sizeof(scheduler->confidence_sum));
    memset(scheduler->executions, 0, sizeof(scheduler->executions));
    memset(&scheduler->active_step, 0, sizeof(scheduler->active_step));

    for (i = 0U; i < node_count; ++i) {
        for (j = i + 1U; j < node_count; ++j) {
            nrf_cs_add_pair(scheduler, i, j);
        }
    }

    scheduler->cycle_started = true;
    return NRF_CS_OK;
}

nrf_cs_result_t nrf_cs_scheduler_next_step(
    nrf_cs_scheduler_t *scheduler,
    nrf_cs_step_t *step_out)
{
    size_t i;

    if (scheduler == NULL || step_out == NULL || !scheduler->cycle_started) {
        return NRF_CS_ERR_ARGUMENT;
    }
    if (scheduler->active) {
        return NRF_CS_ERR_BUSY;
    }

    for (i = 0U; i < scheduler->execution_count; ++i) {
        nrf_cs_execution_t *execution = &scheduler->executions[i];
        nrf_cs_pair_result_t *pair;

        if (execution->terminal) {
            continue;
        }

        ++execution->attempts;
        pair = &scheduler->pairs[execution->pair_index];
        ++pair->attempts;
        scheduler->next_attempt_id = nrf_cs_next_nonzero(scheduler->next_attempt_id);

        scheduler->active_step.cycle_id = scheduler->cycle_id;
        scheduler->active_step.membership_generation =
            scheduler->cycle_membership_generation;
        scheduler->active_step.attempt_id = scheduler->next_attempt_id;
        scheduler->active_step.pair_index = execution->pair_index;
        scheduler->active_step.attempt_number = execution->attempts;
        scheduler->active_step.initiator = execution->initiator;
        scheduler->active_step.reflector = execution->reflector;
        scheduler->active_step.timeout_ms = scheduler->policy.pair_timeout_ms;
        scheduler->active_execution_index = i;
        scheduler->active = true;
        *step_out = scheduler->active_step;
        return NRF_CS_STEP_READY;
    }

    return NRF_CS_CYCLE_COMPLETE;
}

nrf_cs_result_t nrf_cs_scheduler_complete_step(
    nrf_cs_scheduler_t *scheduler,
    const nrf_cs_step_t *step,
    const nrf_cs_measurement_t *measurement)
{
    nrf_cs_execution_t *execution;
    nrf_cs_pair_result_t *pair;
    nrf_cs_failure_reason_t failure;
    bool accepted_success;
    size_t pair_index;

    if (scheduler == NULL || step == NULL || measurement == NULL) {
        return NRF_CS_ERR_ARGUMENT;
    }
    if (measurement->outcome < NRF_CS_OUTCOME_SUCCESS ||
        measurement->outcome > NRF_CS_OUTCOME_OUTLIER ||
        measurement->confidence > NRF_CS_CONFIDENCE_MAX ||
        (measurement->outcome == NRF_CS_OUTCOME_SUCCESS && measurement->distance_mm < 0)) {
        return NRF_CS_ERR_ARGUMENT;
    }
    if (!nrf_cs_execution_matches_step(scheduler, step)) {
        return NRF_CS_ERR_STALE;
    }

    execution = &scheduler->executions[scheduler->active_execution_index];
    pair_index = execution->pair_index;
    pair = &scheduler->pairs[pair_index];
    pair->provider_quality_flags |= measurement->provider_quality_flags;

    accepted_success = measurement->outcome == NRF_CS_OUTCOME_SUCCESS &&
                       measurement->confidence >= scheduler->policy.min_confidence;
    if (accepted_success) {
        execution->terminal = true;
        execution->succeeded = true;
        execution->last_failure = NRF_CS_FAILURE_NONE;
        ++pair->valid_samples;
        scheduler->distance_sum_mm[pair_index] += measurement->distance_mm;
        scheduler->confidence_sum[pair_index] += measurement->confidence;
    } else {
        failure = nrf_cs_failure_from_outcome(measurement->outcome);
        if (measurement->outcome == NRF_CS_OUTCOME_SUCCESS) {
            failure = NRF_CS_FAILURE_OUTLIER;
            pair->scheduler_quality_flags |= NRF_CS_QUALITY_LOW_CONFIDENCE;
        }
        execution->last_failure = failure;
        pair->last_failure = failure;
        nrf_cs_record_failure_quality(pair, failure);

        if (execution->attempts >= scheduler->policy.max_attempts) {
            execution->terminal = true;
            execution->succeeded = false;
        } else {
            pair->scheduler_quality_flags |= NRF_CS_QUALITY_HAD_RETRY;
        }
    }

    scheduler->active = false;
    memset(&scheduler->active_step, 0, sizeof(scheduler->active_step));
    if (execution->terminal) {
        nrf_cs_finalize_pair(scheduler, pair_index);
    }
    return NRF_CS_OK;
}

nrf_cs_result_t nrf_cs_scheduler_disconnect(
    nrf_cs_scheduler_t *scheduler,
    nrf_cs_node_id_t node_id)
{
    size_t node_index = NRF_CS_MAX_NODES;
    size_t i;

    if (scheduler == NULL || !scheduler->cycle_started) {
        return NRF_CS_ERR_ARGUMENT;
    }

    for (i = 0U; i < scheduler->node_count; ++i) {
        if (scheduler->nodes[i] == node_id) {
            node_index = i;
            break;
        }
    }
    if (node_index == NRF_CS_MAX_NODES) {
        return NRF_CS_ERR_NOT_FOUND;
    }
    if ((scheduler->disconnected_mask & (uint8_t)(1U << node_index)) != 0U) {
        return NRF_CS_OK;
    }

    scheduler->disconnected_mask |= (uint8_t)(1U << node_index);
    scheduler->membership_generation =
        nrf_cs_next_nonzero(scheduler->membership_generation);

    if (scheduler->active &&
        nrf_cs_execution_uses_node(
            &scheduler->executions[scheduler->active_execution_index], node_id)) {
        scheduler->active = false;
        memset(&scheduler->active_step, 0, sizeof(scheduler->active_step));
    }

    for (i = 0U; i < scheduler->execution_count; ++i) {
        nrf_cs_execution_t *execution = &scheduler->executions[i];
        nrf_cs_pair_result_t *pair;

        if (execution->terminal || !nrf_cs_execution_uses_node(execution, node_id)) {
            continue;
        }
        execution->terminal = true;
        execution->succeeded = false;
        execution->last_failure = NRF_CS_FAILURE_DISCONNECTED;
        pair = &scheduler->pairs[execution->pair_index];
        pair->last_failure = NRF_CS_FAILURE_DISCONNECTED;
    }

    for (i = 0U; i < scheduler->pair_count; ++i) {
        nrf_cs_finalize_pair(scheduler, i);
    }
    return NRF_CS_OK;
}

bool nrf_cs_scheduler_is_complete(const nrf_cs_scheduler_t *scheduler)
{
    size_t i;

    if (scheduler == NULL || !scheduler->cycle_started || scheduler->active) {
        return false;
    }
    for (i = 0U; i < scheduler->execution_count; ++i) {
        if (!scheduler->executions[i].terminal) {
            return false;
        }
    }
    return true;
}

size_t nrf_cs_scheduler_pair_count(const nrf_cs_scheduler_t *scheduler)
{
    return scheduler == NULL ? 0U : scheduler->pair_count;
}

const nrf_cs_pair_result_t *nrf_cs_scheduler_pair_at(
    const nrf_cs_scheduler_t *scheduler,
    size_t pair_index)
{
    if (scheduler == NULL || pair_index >= scheduler->pair_count) {
        return NULL;
    }
    return &scheduler->pairs[pair_index];
}

const nrf_cs_pair_result_t *nrf_cs_scheduler_matrix_cell(
    const nrf_cs_scheduler_t *scheduler,
    nrf_cs_node_id_t node_a,
    nrf_cs_node_id_t node_b)
{
    nrf_cs_node_id_t low;
    nrf_cs_node_id_t high;
    size_t i;

    if (scheduler == NULL || node_a == node_b) {
        return NULL;
    }
    low = node_a < node_b ? node_a : node_b;
    high = node_a < node_b ? node_b : node_a;

    for (i = 0U; i < scheduler->pair_count; ++i) {
        if (scheduler->pairs[i].node_a == low &&
            scheduler->pairs[i].node_b == high) {
            return &scheduler->pairs[i];
        }
    }
    return NULL;
}
