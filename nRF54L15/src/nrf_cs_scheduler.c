#include "nrf_cs_scheduler.h"

#include <limits.h>
#include <string.h>

static bool nrf_cs_policy_valid(const nrf_cs_policy_t *policy)
{
    return policy != NULL &&
           policy->pair_timeout_ms > 0U &&
           policy->max_attempts > 0U &&
           policy->min_confidence <= NRF_CS_CONFIDENCE_MAX &&
           policy->samples_per_pair <= NRF_CS_MAX_SAMPLES_PER_EXECUTION;
}

static uint8_t nrf_cs_samples_target(const nrf_cs_policy_t *policy)
{
    return policy->samples_per_pair == 0U ? 1U : policy->samples_per_pair;
}

static void nrf_cs_sort_i32(int32_t *values, size_t count)
{
    size_t i;

    for (i = 1U; i < count; ++i) {
        int32_t value = values[i];
        size_t j = i;

        while (j > 0U && values[j - 1U] > value) {
            values[j] = values[j - 1U];
            --j;
        }
        values[j] = value;
    }
}

/* Median of a sorted array; the mean of the middle pair for even counts. */
static int32_t nrf_cs_sorted_median(const int32_t *sorted, size_t count)
{
    if ((count % 2U) == 1U) {
        return sorted[count / 2U];
    }
    return (int32_t)(((int64_t)sorted[count / 2U - 1U] + (int64_t)sorted[count / 2U]) / 2);
}

static int32_t nrf_cs_abs_i32(int32_t value)
{
    return value < 0 ? -value : value;
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

    if (scheduler->policy.full_rotation) {
        nrf_cs_add_execution(scheduler, (uint8_t)pair_index, left, right);
        nrf_cs_add_execution(scheduler, (uint8_t)pair_index, right, left);
        return;
    }

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

typedef struct {
    size_t sample_count;
    int32_t distance_mm;
    uint16_t confidence;
    int32_t spread_mm;
    uint8_t rejected_samples;
} nrf_cs_sample_summary_t;

static bool nrf_cs_summarize_samples(
    const int32_t *distances,
    const uint16_t *confidences,
    size_t count,
    nrf_cs_sample_summary_t *summary)
{
    int32_t sorted[NRF_CS_MAX_SAMPLES_PER_PAIR];
    int32_t deviations[NRF_CS_MAX_SAMPLES_PER_PAIR];
    int32_t kept[NRF_CS_MAX_SAMPLES_PER_PAIR];
    size_t kept_count = 0U;
    uint32_t kept_confidence_sum = 0U;

    if (distances == NULL || confidences == NULL || summary == NULL ||
        count == 0U || count > NRF_CS_MAX_SAMPLES_PER_PAIR) {
        return false;
    }

    memcpy(sorted, distances, count * sizeof(sorted[0]));
    nrf_cs_sort_i32(sorted, count);
    const int32_t median = nrf_cs_sorted_median(sorted, count);
    for (size_t i = 0U; i < count; ++i) {
        deviations[i] = nrf_cs_abs_i32(distances[i] - median);
    }
    nrf_cs_sort_i32(deviations, count);
    const int32_t mad = nrf_cs_sorted_median(deviations, count);
    /* 64-bit: a legal wire distance can reach INT32_MAX mm, where 3 * sigma
     * overflows int32 and used to reject every sample (then divide by zero). */
    const int64_t sigma = ((int64_t)mad * 14826 + 5000) / 10000;
    int64_t band = 3 * sigma;
    if (band < NRF_CS_MIN_OUTLIER_BAND_MM) {
        band = NRF_CS_MIN_OUTLIER_BAND_MM;
    }

    for (size_t i = 0U; i < count; ++i) {
        if ((int64_t)nrf_cs_abs_i32(distances[i] - median) <= band) {
            kept[kept_count++] = distances[i];
            kept_confidence_sum += confidences[i];
        }
    }
    nrf_cs_sort_i32(kept, kept_count);

    summary->sample_count = count;
    summary->distance_mm = nrf_cs_sorted_median(kept, kept_count);
    summary->confidence = (uint16_t)(kept_confidence_sum / kept_count);
    summary->spread_mm = count > 1U
        ? (sigma > INT32_MAX ? INT32_MAX : (int32_t)(sigma < NRF_CS_MIN_SPREAD_MM ? NRF_CS_MIN_SPREAD_MM : sigma))
        : 0;
    summary->rejected_samples = (uint8_t)(count - kept_count);
    return true;
}

static void nrf_cs_finalize_pair(nrf_cs_scheduler_t *scheduler, size_t pair_index)
{
    nrf_cs_pair_result_t *pair = &scheduler->pairs[pair_index];
    bool any_failure = false;
    bool any_disconnect = false;
    int32_t distances[NRF_CS_MAX_SAMPLES_PER_PAIR];
    uint16_t confidences[NRF_CS_MAX_SAMPLES_PER_PAIR];
    size_t sample_count = 0U;
    nrf_cs_sample_summary_t directions[2];
    size_t direction_count = 0U;
    const uint8_t target = nrf_cs_samples_target(&scheduler->policy);
    const size_t required = target < NRF_CS_MIN_SAMPLES_PER_DIRECTION ? target : NRF_CS_MIN_SAMPLES_PER_DIRECTION;

    for (size_t i = 0U; i < scheduler->execution_count; ++i) {
        const nrf_cs_execution_t *execution = &scheduler->executions[i];

        if (execution->pair_index != pair_index) {
            continue;
        }
        if (!execution->terminal) {
            return;
        }
    }

    pair->valid_samples = 0U;
    pair->distance_mm = 0;
    pair->confidence = 0U;
    pair->spread_mm = 0;
    pair->rejected_samples = 0U;
    pair->single_direction = false;
    pair->direction_disagreement_mm = 0;

    for (size_t i = 0U; i < scheduler->execution_count; ++i) {
        const nrf_cs_execution_t *execution = &scheduler->executions[i];
        if (execution->pair_index != pair_index) {
            continue;
        }
        if (!execution->succeeded) {
            any_failure = true;
            if (execution->last_failure == NRF_CS_FAILURE_DISCONNECTED) {
                any_disconnect = true;
            }
        }
        if (scheduler->policy.full_rotation && execution->successes < required) {
            any_failure = true;     /* too few samples to trust: this direction is left out of the pair */
            continue;
        }
        for (size_t sample = 0U; sample < execution->successes; ++sample) {
            distances[sample_count] = scheduler->sample_distance_mm[i][sample];
            confidences[sample_count] = scheduler->sample_confidence[i][sample];
            ++sample_count;
        }
        if (scheduler->policy.full_rotation && execution->successes > 0U) {
            if (direction_count >= sizeof(directions) / sizeof(directions[0]) ||
                !nrf_cs_summarize_samples(
                    scheduler->sample_distance_mm[i],
                    scheduler->sample_confidence[i],
                    execution->successes,
                    &directions[direction_count])) {
                return;
            }
            ++direction_count;
        }
    }

    if (scheduler->policy.full_rotation) {
        if (direction_count > 0U) {
            const nrf_cs_sample_summary_t *first = &directions[0];
            pair->state = NRF_CS_PAIR_VALID;
            pair->valid_samples = (uint8_t)sample_count;
            pair->distance_mm = first->distance_mm;
            pair->confidence = first->confidence;
            pair->spread_mm = first->spread_mm;
            pair->rejected_samples = first->rejected_samples;
            if (direction_count == 2U) {
                const nrf_cs_sample_summary_t *second = &directions[1];
                const int64_t difference =
                    (int64_t)first->distance_mm - (int64_t)second->distance_mm;
                const int64_t disagreement = difference < 0 ? -difference : difference;
                const int64_t spread =
                    ((int64_t)first->spread_mm + second->spread_mm + 1) / 2 + disagreement;
                pair->distance_mm = (int32_t)(
                    ((int64_t)first->distance_mm + second->distance_mm) / 2);
                pair->confidence = (uint16_t)(
                    ((uint32_t)first->confidence + second->confidence) / 2U);
                pair->spread_mm = spread > INT32_MAX ? INT32_MAX : (int32_t)spread;
                pair->direction_disagreement_mm = disagreement > INT32_MAX
                    ? INT32_MAX
                    : (int32_t)disagreement;
                if (disagreement > NRF_CS_DIRECTION_DISAGREEMENT_LIMIT_MM) {
                    /* The mean of two contradicting values is not a measurement; the numbers stay for the log. */
                    pair->state = NRF_CS_PAIR_DIRECTIONS_DISAGREE;
                }
                pair->rejected_samples = (uint8_t)(
                    first->rejected_samples + second->rejected_samples);
            } else {
                pair->single_direction = true;
                pair->scheduler_quality_flags |= NRF_CS_QUALITY_PARTIAL;
            }
            if (any_failure) {
                pair->scheduler_quality_flags |= NRF_CS_QUALITY_PARTIAL;
            }
            if (pair->rejected_samples > 0U) {
                pair->scheduler_quality_flags |= NRF_CS_QUALITY_OUTLIER;
            }
            return;
        }
    } else if (sample_count >= required && sample_count > 0U) {
        nrf_cs_sample_summary_t summary;
        if (!nrf_cs_summarize_samples(distances, confidences, sample_count, &summary)) {
            return;
        }
        pair->state = NRF_CS_PAIR_VALID;
        pair->valid_samples = (uint8_t)summary.sample_count;
        pair->distance_mm = summary.distance_mm;
        pair->confidence = summary.confidence;
        pair->spread_mm = summary.spread_mm;
        pair->rejected_samples = summary.rejected_samples;
        if (any_failure) {
            pair->scheduler_quality_flags |= NRF_CS_QUALITY_PARTIAL;
        }
        if (pair->rejected_samples > 0U) {
            pair->scheduler_quality_flags |= NRF_CS_QUALITY_OUTLIER;
        }
        return;
    }

    if (any_disconnect) {
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
    memset(scheduler->sample_distance_mm, 0, sizeof(scheduler->sample_distance_mm));
    memset(scheduler->sample_confidence, 0, sizeof(scheduler->sample_confidence));
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
        execution->last_failure = NRF_CS_FAILURE_NONE;
        if (execution->successes < NRF_CS_MAX_SAMPLES_PER_EXECUTION) {
            scheduler->sample_distance_mm[scheduler->active_execution_index][execution->successes] = measurement->distance_mm;
            scheduler->sample_confidence[scheduler->active_execution_index][execution->successes] = measurement->confidence;
        }
        ++execution->successes;
        execution->succeeded = true;
        ++pair->valid_samples;
        if (execution->successes >= nrf_cs_samples_target(&scheduler->policy)) {
            execution->terminal = true;
        }
    } else {
        failure = nrf_cs_failure_from_outcome(measurement->outcome);
        if (measurement->outcome == NRF_CS_OUTCOME_SUCCESS) {
            failure = NRF_CS_FAILURE_OUTLIER;
            pair->scheduler_quality_flags |= NRF_CS_QUALITY_LOW_CONFIDENCE;
        }
        execution->last_failure = failure;
        pair->last_failure = failure;
        nrf_cs_record_failure_quality(pair, failure);

        /* Only failed attempts consume the retry budget; the successes an
         * execution already collected are kept (a partial sample set). */
        if ((uint8_t)(execution->attempts - execution->successes) >= scheduler->policy.max_attempts) {
            execution->terminal = true;
            execution->succeeded = execution->successes > 0U;
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

nrf_cs_result_t nrf_cs_scheduler_abort_cycle(
    nrf_cs_scheduler_t *scheduler,
    nrf_cs_failure_reason_t reason)
{
    if (scheduler == NULL || !scheduler->cycle_started ||
        reason < NRF_CS_FAILURE_TIMEOUT || reason > NRF_CS_FAILURE_DISCONNECTED) {
        return NRF_CS_ERR_ARGUMENT;
    }
    if (scheduler->active) {
        scheduler->active = false;
        memset(&scheduler->active_step, 0, sizeof(scheduler->active_step));
    }
    for (size_t i = 0U; i < scheduler->execution_count; ++i) {
        nrf_cs_execution_t *execution = &scheduler->executions[i];
        if (execution->terminal) {
            continue;
        }
        execution->terminal = true;
        execution->succeeded = execution->successes > 0U;
        execution->last_failure = reason;
        nrf_cs_pair_result_t *pair = &scheduler->pairs[execution->pair_index];
        pair->last_failure = reason;
        nrf_cs_record_failure_quality(pair, reason);
    }
    for (size_t i = 0U; i < scheduler->pair_count; ++i) {
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

size_t nrf_cs_scheduler_execution_count(const nrf_cs_scheduler_t *scheduler)
{
    return scheduler == NULL ? 0U : scheduler->execution_count;
}

size_t nrf_cs_scheduler_executions_done(const nrf_cs_scheduler_t *scheduler)
{
    size_t done = 0U;
    if (scheduler == NULL) {
        return 0U;
    }
    for (size_t i = 0U; i < scheduler->execution_count; ++i) {
        if (scheduler->executions[i].terminal) {
            ++done;
        }
    }
    return done;
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
