#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "nrf_cs_scheduler.h"

static nrf_cs_policy_t test_policy(uint8_t max_attempts)
{
    nrf_cs_policy_t policy;
    policy.pair_timeout_ms = 120U;
    policy.max_attempts = max_attempts;
    policy.min_confidence = 600U;
    policy.samples_per_pair = 1U;
    policy.full_rotation = false;
    policy.relay_order = false;
    return policy;
}

static nrf_cs_measurement_t success_measurement(int32_t distance_mm, uint16_t confidence)
{
    nrf_cs_measurement_t measurement;
    measurement.outcome = NRF_CS_OUTCOME_SUCCESS;
    measurement.distance_mm = distance_mm;
    measurement.confidence = confidence;
    measurement.provider_quality_flags = 0U;
    return measurement;
}

static nrf_cs_measurement_t failure_measurement(nrf_cs_outcome_t outcome)
{
    nrf_cs_measurement_t measurement;
    measurement.outcome = outcome;
    measurement.distance_mm = 0;
    measurement.confidence = 0U;
    measurement.provider_quality_flags = 0U;
    return measurement;
}

static void complete_all_success(
    nrf_cs_scheduler_t *scheduler,
    unsigned *initiator_counts,
    const nrf_cs_node_id_t *ordered_nodes,
    size_t node_count)
{
    nrf_cs_step_t step;
    nrf_cs_measurement_t measurement = success_measurement(1000, 900U);

    while (nrf_cs_scheduler_next_step(scheduler, &step) == NRF_CS_STEP_READY) {
        size_t i;
        bool found = false;
        for (i = 0U; i < node_count; ++i) {
            if (ordered_nodes[i] == step.initiator) {
                ++initiator_counts[i];
                found = true;
                break;
            }
        }
        assert(found);
        assert(nrf_cs_scheduler_complete_step(scheduler, &step, &measurement) == NRF_CS_OK);
    }
    assert(nrf_cs_scheduler_is_complete(scheduler));
}

static void test_pair_enumeration_is_canonical_and_discovery_order_independent(void)
{
    nrf_cs_scheduler_t a;
    nrf_cs_scheduler_t b;
    nrf_cs_policy_t policy = test_policy(2U);
    const nrf_cs_node_id_t unsorted[] = { 42U, 3U, 19U, 7U, 88U };
    const nrf_cs_node_id_t sorted[] = { 3U, 7U, 19U, 42U, 88U };
    size_t i;

    assert(nrf_cs_scheduler_init(&a, &policy) == NRF_CS_OK);
    assert(nrf_cs_scheduler_init(&b, &policy) == NRF_CS_OK);
    assert(nrf_cs_scheduler_start_cycle(&a, unsorted, 5U) == NRF_CS_OK);
    assert(nrf_cs_scheduler_start_cycle(&b, sorted, 5U) == NRF_CS_OK);
    assert(nrf_cs_scheduler_pair_count(&a) == 10U);
    assert(nrf_cs_scheduler_pair_count(&b) == 10U);

    for (i = 0U; i < 10U; ++i) {
        const nrf_cs_pair_result_t *left = nrf_cs_scheduler_pair_at(&a, i);
        const nrf_cs_pair_result_t *right = nrf_cs_scheduler_pair_at(&b, i);
        assert(left != NULL && right != NULL);
        assert(left->node_a == right->node_a);
        assert(left->node_b == right->node_b);
        assert(left->node_a < left->node_b);
        assert(left->state == NRF_CS_PAIR_PENDING);
    }

    assert(nrf_cs_scheduler_matrix_cell(&a, 88U, 3U) ==
           nrf_cs_scheduler_matrix_cell(&a, 3U, 88U));
    assert(nrf_cs_scheduler_matrix_cell(&a, 3U, 3U) == NULL);
}

static void test_initiator_fairness_for_three_four_and_five_nodes(void)
{
    const nrf_cs_node_id_t n3[] = { 1U, 2U, 3U };
    const nrf_cs_node_id_t n4[] = { 1U, 2U, 3U, 4U };
    const nrf_cs_node_id_t n5[] = { 1U, 2U, 3U, 4U, 5U };
    const nrf_cs_node_id_t *sets[] = { n3, n4, n5 };
    const size_t counts[] = { 3U, 4U, 5U };
    size_t case_index;

    for (case_index = 0U; case_index < 3U; ++case_index) {
        nrf_cs_scheduler_t scheduler;
        nrf_cs_policy_t policy = test_policy(1U);
        unsigned initiator_counts[NRF_CS_MAX_NODES] = { 0U };
        size_t i;

        assert(nrf_cs_scheduler_init(&scheduler, &policy) == NRF_CS_OK);
        assert(nrf_cs_scheduler_start_cycle(
                   &scheduler, sets[case_index], counts[case_index]) == NRF_CS_OK);
        complete_all_success(
            &scheduler, initiator_counts, sets[case_index], counts[case_index]);

        for (i = 0U; i < counts[case_index]; ++i) {
            assert(initiator_counts[i] > 0U);
        }
        if (counts[case_index] == 3U) {
            assert(initiator_counts[0] == 1U);
            assert(initiator_counts[1] == 1U);
            assert(initiator_counts[2] == 1U);
        }
        if (counts[case_index] == 4U) {
            unsigned twos = 0U;
            unsigned ones = 0U;
            for (i = 0U; i < 4U; ++i) {
                twos += initiator_counts[i] == 2U ? 1U : 0U;
                ones += initiator_counts[i] == 1U ? 1U : 0U;
            }
            assert(twos == 2U);
            assert(ones == 2U);
        }
        if (counts[case_index] == 5U) {
            for (i = 0U; i < 5U; ++i) {
                assert(initiator_counts[i] == 2U);
            }
        }
    }
}

static void test_two_nodes_get_reciprocal_directed_steps_for_one_matrix_pair(void)
{
    nrf_cs_scheduler_t scheduler;
    nrf_cs_policy_t policy = test_policy(1U);
    const nrf_cs_node_id_t nodes[] = { 9U, 4U };
    nrf_cs_step_t first;
    nrf_cs_step_t second;
    nrf_cs_measurement_t m1 = success_measurement(1000, 800U);
    nrf_cs_measurement_t m2 = success_measurement(1200, 1000U);
    const nrf_cs_pair_result_t *pair;

    assert(nrf_cs_scheduler_init(&scheduler, &policy) == NRF_CS_OK);
    assert(nrf_cs_scheduler_start_cycle(&scheduler, nodes, 2U) == NRF_CS_OK);
    assert(nrf_cs_scheduler_pair_count(&scheduler) == 1U);

    assert(nrf_cs_scheduler_next_step(&scheduler, &first) == NRF_CS_STEP_READY);
    assert(first.initiator == 4U && first.reflector == 9U);
    assert(nrf_cs_scheduler_complete_step(&scheduler, &first, &m1) == NRF_CS_OK);
    assert(nrf_cs_scheduler_next_step(&scheduler, &second) == NRF_CS_STEP_READY);
    assert(second.initiator == 9U && second.reflector == 4U);
    assert(first.pair_index == second.pair_index);
    assert(nrf_cs_scheduler_complete_step(&scheduler, &second, &m2) == NRF_CS_OK);
    assert(nrf_cs_scheduler_next_step(&scheduler, &second) == NRF_CS_CYCLE_COMPLETE);

    pair = nrf_cs_scheduler_pair_at(&scheduler, 0U);
    assert(pair != NULL);
    assert(pair->state == NRF_CS_PAIR_VALID);
    assert(pair->valid_samples == 2U);
    assert(pair->attempts == 2U);
    assert(pair->distance_mm == 1100);
    assert(pair->confidence == 900U);
}

static void test_one_active_step_and_stale_attempt_tokens(void)
{
    nrf_cs_scheduler_t scheduler;
    nrf_cs_policy_t policy = test_policy(2U);
    const nrf_cs_node_id_t nodes[] = { 1U, 2U, 3U };
    nrf_cs_step_t step;
    nrf_cs_step_t stale;
    nrf_cs_measurement_t timeout = failure_measurement(NRF_CS_OUTCOME_TIMEOUT);
    nrf_cs_measurement_t success = success_measurement(500, 900U);

    assert(nrf_cs_scheduler_init(&scheduler, &policy) == NRF_CS_OK);
    assert(nrf_cs_scheduler_start_cycle(&scheduler, nodes, 3U) == NRF_CS_OK);
    assert(nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY);
    stale = step;
    assert(nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_ERR_BUSY);
    assert(nrf_cs_scheduler_complete_step(&scheduler, &stale, &timeout) == NRF_CS_OK);

    assert(nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY);
    assert(step.attempt_number == 2U);
    assert(step.attempt_id != stale.attempt_id);
    assert(nrf_cs_scheduler_complete_step(&scheduler, &stale, &success) == NRF_CS_ERR_STALE);
    assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &success) == NRF_CS_OK);
}

static void test_timeout_retry_exhaustion_is_terminal_and_explicit(void)
{
    nrf_cs_scheduler_t scheduler;
    nrf_cs_policy_t policy = test_policy(2U);
    const nrf_cs_node_id_t nodes[] = { 1U, 2U, 3U };
    nrf_cs_measurement_t timeout = failure_measurement(NRF_CS_OUTCOME_TIMEOUT);
    nrf_cs_step_t step;
    const nrf_cs_pair_result_t *pair;

    assert(nrf_cs_scheduler_init(&scheduler, &policy) == NRF_CS_OK);
    assert(nrf_cs_scheduler_start_cycle(&scheduler, nodes, 3U) == NRF_CS_OK);
    assert(nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY);
    assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &timeout) == NRF_CS_OK);
    assert(nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY);
    assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &timeout) == NRF_CS_OK);

    pair = nrf_cs_scheduler_pair_at(&scheduler, 0U);
    assert(pair != NULL);
    assert(pair->state == NRF_CS_PAIR_RETRY_EXHAUSTED);
    assert(pair->last_failure == NRF_CS_FAILURE_TIMEOUT);
    assert(pair->attempts == 2U);
    assert((pair->scheduler_quality_flags & NRF_CS_QUALITY_HAD_RETRY) != 0U);
    assert((pair->scheduler_quality_flags & NRF_CS_QUALITY_TIMEOUT) != 0U);
}

static void test_low_confidence_success_retries_as_outlier(void)
{
    nrf_cs_scheduler_t scheduler;
    nrf_cs_policy_t policy = test_policy(2U);
    const nrf_cs_node_id_t nodes[] = { 1U, 2U, 3U };
    nrf_cs_step_t step;
    nrf_cs_measurement_t low = success_measurement(900, 599U);
    nrf_cs_measurement_t good = success_measurement(1000, 850U);
    const nrf_cs_pair_result_t *pair;

    assert(nrf_cs_scheduler_init(&scheduler, &policy) == NRF_CS_OK);
    assert(nrf_cs_scheduler_start_cycle(&scheduler, nodes, 3U) == NRF_CS_OK);
    assert(nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY);
    assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &low) == NRF_CS_OK);
    assert(nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY);
    assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &good) == NRF_CS_OK);

    pair = nrf_cs_scheduler_pair_at(&scheduler, 0U);
    assert(pair != NULL && pair->state == NRF_CS_PAIR_VALID);
    assert(pair->valid_samples == 1U);
    assert(pair->distance_mm == 1000);
    assert((pair->scheduler_quality_flags & NRF_CS_QUALITY_LOW_CONFIDENCE) != 0U);
    assert((pair->scheduler_quality_flags & NRF_CS_QUALITY_OUTLIER) != 0U);
}

static void test_disconnect_cancels_only_affected_unfinished_pairs_and_rejoin_is_next_cycle(void)
{
    nrf_cs_scheduler_t scheduler;
    nrf_cs_policy_t policy = test_policy(2U);
    const nrf_cs_node_id_t nodes[] = { 1U, 2U, 3U, 4U };
    nrf_cs_step_t canceled_step;
    nrf_cs_step_t step;
    nrf_cs_measurement_t success = success_measurement(1500, 900U);
    uint32_t first_cycle;
    uint32_t first_generation;
    size_t i;

    assert(nrf_cs_scheduler_init(&scheduler, &policy) == NRF_CS_OK);
    assert(nrf_cs_scheduler_start_cycle(&scheduler, nodes, 4U) == NRF_CS_OK);
    first_cycle = scheduler.cycle_id;
    first_generation = scheduler.cycle_membership_generation;

    assert(nrf_cs_scheduler_next_step(&scheduler, &canceled_step) == NRF_CS_STEP_READY);
    assert(canceled_step.initiator == 1U || canceled_step.reflector == 1U);
    assert(nrf_cs_scheduler_disconnect(&scheduler, 1U) == NRF_CS_OK);
    assert(nrf_cs_scheduler_complete_step(&scheduler, &canceled_step, &success) == NRF_CS_ERR_STALE);

    while (nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY) {
        assert(step.initiator != 1U && step.reflector != 1U);
        assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &success) == NRF_CS_OK);
    }
    assert(nrf_cs_scheduler_is_complete(&scheduler));

    for (i = 0U; i < scheduler.pair_count; ++i) {
        const nrf_cs_pair_result_t *pair = nrf_cs_scheduler_pair_at(&scheduler, i);
        assert(pair != NULL);
        if (pair->node_a == 1U || pair->node_b == 1U) {
            assert(pair->state == NRF_CS_PAIR_MISSING);
        } else {
            assert(pair->state == NRF_CS_PAIR_VALID);
        }
    }

    assert(nrf_cs_scheduler_start_cycle(&scheduler, nodes, 4U) == NRF_CS_OK);
    assert(scheduler.cycle_id != first_cycle);
    assert(scheduler.cycle_membership_generation != first_generation);
    for (i = 0U; i < scheduler.pair_count; ++i) {
        const nrf_cs_pair_result_t *pair = nrf_cs_scheduler_pair_at(&scheduler, i);
        assert(pair != NULL && pair->state == NRF_CS_PAIR_PENDING);
        assert(pair->valid_samples == 0U);
    }
}

static void test_main_identity_has_no_privileged_initiator_role(void)
{
    nrf_cs_scheduler_t scheduler;
    nrf_cs_policy_t policy = test_policy(1U);
    const nrf_cs_node_id_t nodes[] = { 10U, 20U, 30U, 40U, 50U };
    nrf_cs_step_t step;
    nrf_cs_measurement_t success = success_measurement(2000, 900U);
    bool main_initiates = false;
    bool main_reflects = false;

    assert(nrf_cs_scheduler_init(&scheduler, &policy) == NRF_CS_OK);
    assert(nrf_cs_scheduler_start_cycle(&scheduler, nodes, 5U) == NRF_CS_OK);
    while (nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY) {
        if (step.initiator == 10U) main_initiates = true;
        if (step.reflector == 10U) main_reflects = true;
        assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &success) == NRF_CS_OK);
    }
    assert(main_initiates);
    assert(main_reflects);
}

static void test_empty_and_single_node_cycles_complete_without_steps(void)
{
    nrf_cs_scheduler_t scheduler;
    nrf_cs_policy_t policy = test_policy(1U);
    const nrf_cs_node_id_t one[] = { 7U };
    nrf_cs_step_t step;

    assert(nrf_cs_scheduler_init(&scheduler, &policy) == NRF_CS_OK);
    assert(nrf_cs_scheduler_start_cycle(&scheduler, NULL, 0U) == NRF_CS_OK);
    assert(nrf_cs_scheduler_is_complete(&scheduler));
    assert(nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_CYCLE_COMPLETE);

    assert(nrf_cs_scheduler_start_cycle(&scheduler, one, 1U) == NRF_CS_OK);
    assert(nrf_cs_scheduler_pair_count(&scheduler) == 0U);
    assert(nrf_cs_scheduler_is_complete(&scheduler));
    assert(nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_CYCLE_COMPLETE);
}

static void test_invalid_inputs_are_rejected(void)
{
    nrf_cs_scheduler_t scheduler;
    nrf_cs_policy_t policy = test_policy(1U);
    nrf_cs_policy_t bad_policy = policy;
    const nrf_cs_node_id_t duplicate[] = { 1U, 1U };
    const nrf_cs_node_id_t too_many[] = { 1U, 2U, 3U, 4U, 5U, 6U };

    bad_policy.max_attempts = 0U;
    assert(nrf_cs_scheduler_init(NULL, &policy) == NRF_CS_ERR_ARGUMENT);
    assert(nrf_cs_scheduler_init(&scheduler, &bad_policy) == NRF_CS_ERR_ARGUMENT);
    assert(nrf_cs_scheduler_init(&scheduler, &policy) == NRF_CS_OK);
    assert(nrf_cs_scheduler_start_cycle(&scheduler, duplicate, 2U) == NRF_CS_ERR_ARGUMENT);
    assert(nrf_cs_scheduler_start_cycle(&scheduler, too_many, 6U) == NRF_CS_ERR_ARGUMENT);
    assert(nrf_cs_scheduler_start_cycle(&scheduler, NULL, 1U) == NRF_CS_ERR_ARGUMENT);
}

/* MAX-206: one step per pair ran until the first success; the pair's
 * distance was that single sample. With samples_per_pair the pair keeps
 * sampling, takes the median and discards outliers by MAD. */
static void run_single_pair_samples(
    const int32_t *distances,
    size_t count,
    nrf_cs_pair_result_t *out)
{
    nrf_cs_scheduler_t scheduler;
    nrf_cs_policy_t policy = test_policy(2U);
    const nrf_cs_node_id_t nodes[] = { 0U, 1U, 2U };
    nrf_cs_step_t step;
    size_t served = 0U;

    policy.samples_per_pair = (uint8_t)count;
    assert(nrf_cs_scheduler_init(&scheduler, &policy) == NRF_CS_OK);
    assert(nrf_cs_scheduler_start_cycle(&scheduler, nodes, 3U) == NRF_CS_OK);
    while (nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY) {
        /* Pair 0 gets the scripted distances; other pairs a constant. */
        const int32_t distance = step.pair_index == 0U ? distances[served++] : 2000;
        const nrf_cs_measurement_t measurement = success_measurement(distance, 900U);
        assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &measurement) == NRF_CS_OK);
    }
    assert(served == count);
    assert(nrf_cs_scheduler_is_complete(&scheduler));
    *out = *nrf_cs_scheduler_pair_at(&scheduler, 0U);
}

static void test_multi_sample_median_rejects_outlier(void)
{
    const int32_t distances[5] = { 3010, 2990, 3005, 5400, 2995 };
    nrf_cs_pair_result_t pair;
    run_single_pair_samples(distances, 5U, &pair);

    assert(pair.state == NRF_CS_PAIR_VALID);
    assert(pair.valid_samples == 5U);
    assert(pair.rejected_samples == 1U);
    assert(pair.distance_mm == 3000);           /* median of the 4 kept */
    assert(pair.spread_mm > 0 && pair.spread_mm < 50);
    assert((pair.scheduler_quality_flags & NRF_CS_QUALITY_OUTLIER) != 0U);
    assert(pair.confidence == 900U);
}

static void test_single_sample_policy_keeps_legacy_behaviour(void)
{
    const int32_t distances[1] = { 4321 };
    nrf_cs_pair_result_t pair;
    run_single_pair_samples(distances, 1U, &pair);
    assert(pair.valid_samples == 1U);
    assert(pair.distance_mm == 4321);
    assert(pair.spread_mm == 0);
    assert(pair.rejected_samples == 0U);
}

static void test_samples_per_pair_is_bounded(void)
{
    nrf_cs_scheduler_t scheduler;
    nrf_cs_policy_t policy = test_policy(1U);
    policy.samples_per_pair = NRF_CS_MAX_SAMPLES_PER_EXECUTION + 1U;
    assert(nrf_cs_scheduler_init(&scheduler, &policy) == NRF_CS_ERR_ARGUMENT);
}

static void test_full_rotation_has_both_orders_in_canonical_sequence(void)
{
    const nrf_cs_node_id_t nodes5[] = { 5U, 2U, 4U, 1U, 3U };
    const nrf_cs_node_id_t *node_sets[] = {
        (const nrf_cs_node_id_t[]){ 3U, 1U, 2U },
        (const nrf_cs_node_id_t[]){ 4U, 1U, 3U, 2U },
        nodes5,
    };
    const size_t counts[] = { 3U, 4U, 5U };

    for (size_t case_index = 0U; case_index < 3U; ++case_index) {
        nrf_cs_scheduler_t scheduler;
        nrf_cs_policy_t policy = test_policy(1U);
        const nrf_cs_node_id_t *nodes = node_sets[case_index];
        const size_t node_count = counts[case_index];
        unsigned initiators[NRF_CS_MAX_NODES] = { 0U };
        size_t ordered_execution = 0U;
        policy.full_rotation = true;

        assert(nrf_cs_scheduler_init(&scheduler, &policy) == NRF_CS_OK);
        assert(nrf_cs_scheduler_start_cycle(&scheduler, nodes, node_count) == NRF_CS_OK);
        assert(nrf_cs_scheduler_pair_count(&scheduler) == node_count * (node_count - 1U) / 2U);
        assert(nrf_cs_scheduler_execution_count(&scheduler) == node_count * (node_count - 1U));
        assert(nrf_cs_scheduler_execution_count(&scheduler) <= NRF_CS_MAX_EXECUTIONS);

        for (size_t left = 1U; left <= node_count; ++left) {
            for (size_t right = left + 1U; right <= node_count; ++right) {
                nrf_cs_step_t step;
                const nrf_cs_node_id_t a = (nrf_cs_node_id_t)left;
                const nrf_cs_node_id_t b = (nrf_cs_node_id_t)right;
                const nrf_cs_node_id_t expected_initiators[] = { a, b };
                const nrf_cs_node_id_t expected_reflectors[] = { b, a };
                for (size_t direction = 0U; direction < 2U; ++direction) {
                    assert(nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY);
                    assert(step.pair_index == ordered_execution / 2U);
                    assert(step.initiator == expected_initiators[direction]);
                    assert(step.reflector == expected_reflectors[direction]);
                    ++initiators[step.initiator - 1U];
                    ++ordered_execution;
                    const nrf_cs_measurement_t measurement = success_measurement(1000, 900U);
                    assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &measurement) == NRF_CS_OK);
                }
            }
        }

        nrf_cs_step_t finished;
        assert(nrf_cs_scheduler_next_step(&scheduler, &finished) == NRF_CS_CYCLE_COMPLETE);
        assert(ordered_execution == node_count * (node_count - 1U));
        assert(nrf_cs_scheduler_executions_done(&scheduler) == ordered_execution);
        for (size_t node = 0U; node < node_count; ++node) {
            assert(initiators[node] == node_count - 1U);
        }
    }
}

static void test_full_rotation_pair_combine_and_direction_failures(void)
{
    nrf_cs_scheduler_t scheduler;
    nrf_cs_policy_t policy = test_policy(1U);
    const nrf_cs_node_id_t nodes[] = { 0U, 1U, 2U };
    nrf_cs_step_t step;
    nrf_cs_pair_result_t pair;

    policy.full_rotation = true;
    policy.samples_per_pair = 3U;
    assert(nrf_cs_scheduler_init(&scheduler, &policy) == NRF_CS_OK);
    assert(nrf_cs_scheduler_start_cycle(&scheduler, nodes, 3U) == NRF_CS_OK);
    const int32_t forward[] = { 1000, 1010, 990 };
    const int32_t reverse[] = { 1200, 1210, 1190 };
    for (size_t direction = 0U; direction < 2U; ++direction) {
        const int32_t *distances = direction == 0U ? forward : reverse;
        for (size_t sample = 0U; sample < 3U; ++sample) {
            assert(nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY);
            assert(step.pair_index == 0U);
            const nrf_cs_measurement_t measurement = success_measurement(
                distances[sample], direction == 0U ? 900U : 700U);
            assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &measurement) == NRF_CS_OK);
        }
    }
    pair = *nrf_cs_scheduler_pair_at(&scheduler, 0U);
    assert(pair.state == NRF_CS_PAIR_VALID);
    assert(pair.distance_mm == 1100);
    assert(pair.confidence == 800U);
    assert(pair.valid_samples == 6U);
    assert(pair.direction_disagreement_mm == 200);
    /* sigma of 10 mm MAD is 15 mm, below the floor of NRF_CS_MIN_SPREAD_MM per direction: (33 + 33 + 1) / 2 + 200 */
    assert(NRF_CS_MIN_SPREAD_MM == 33);
    assert(pair.spread_mm == 233);
    assert(!pair.single_direction);

    policy.samples_per_pair = 1U;
    assert(nrf_cs_scheduler_init(&scheduler, &policy) == NRF_CS_OK);
    assert(nrf_cs_scheduler_start_cycle(&scheduler, nodes, 3U) == NRF_CS_OK);
    assert(nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY);
    nrf_cs_measurement_t measurement = success_measurement(1234, 900U);
    assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &measurement) == NRF_CS_OK);
    assert(nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY);
    measurement = failure_measurement(NRF_CS_OUTCOME_TIMEOUT);
    assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &measurement) == NRF_CS_OK);
    pair = *nrf_cs_scheduler_pair_at(&scheduler, 0U);
    assert(pair.state == NRF_CS_PAIR_VALID);
    assert(pair.distance_mm == 1234);
    assert(pair.valid_samples == 1U);
    assert(pair.single_direction);
    assert(pair.spread_mm == 0);
    assert(pair.direction_disagreement_mm == 0);
    assert((pair.scheduler_quality_flags & NRF_CS_QUALITY_PARTIAL) != 0U);

    assert(nrf_cs_scheduler_init(&scheduler, &policy) == NRF_CS_OK);
    assert(nrf_cs_scheduler_start_cycle(&scheduler, nodes, 3U) == NRF_CS_OK);
    const nrf_cs_measurement_t timeout = failure_measurement(NRF_CS_OUTCOME_TIMEOUT);
    for (size_t direction = 0U; direction < 2U; ++direction) {
        assert(nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY);
        assert(step.pair_index == 0U);
        assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &timeout) == NRF_CS_OK);
    }
    pair = *nrf_cs_scheduler_pair_at(&scheduler, 0U);
    assert(pair.state == NRF_CS_PAIR_RETRY_EXHAUSTED);
    assert(pair.last_failure == NRF_CS_FAILURE_TIMEOUT);
    assert(pair.valid_samples == 0U);
    assert(!pair.single_direction);
}

/* Audit 2026-11-02: two directions that contradict each other (1 m one way, 9 m the other) used to be averaged into a
 * "valid" 5 m. Now the pair is not published as a distance; the numbers stay so the log shows what happened. */
static nrf_cs_pair_result_t run_two_directions(size_t samples, const int32_t *forward, const int32_t *reverse)
{
    nrf_cs_scheduler_t scheduler;
    nrf_cs_policy_t policy = test_policy(1U);
    const nrf_cs_node_id_t nodes[] = { 0U, 1U, 2U };
    nrf_cs_step_t step;

    policy.full_rotation = true;
    policy.samples_per_pair = (uint8_t)samples;
    assert(nrf_cs_scheduler_init(&scheduler, &policy) == NRF_CS_OK);
    assert(nrf_cs_scheduler_start_cycle(&scheduler, nodes, 3U) == NRF_CS_OK);
    for (size_t direction = 0U; direction < 2U; ++direction) {
        const int32_t *distances = direction == 0U ? forward : reverse;
        for (size_t sample = 0U; sample < samples; ++sample) {
            assert(nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY);
            assert(step.pair_index == 0U);
            const nrf_cs_measurement_t measurement = success_measurement(distances[sample], 900U);
            assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &measurement) == NRF_CS_OK);
        }
    }
    return *nrf_cs_scheduler_pair_at(&scheduler, 0U);
}

static void test_contradicting_directions_are_not_a_valid_pair(void)
{
    const int32_t forward[] = { 990, 1000, 1010 };
    const int32_t reverse[] = { 8990, 9000, 9010 };
    nrf_cs_pair_result_t pair = run_two_directions(3U, forward, reverse);

    assert(pair.state == NRF_CS_PAIR_DIRECTIONS_DISAGREE);
    assert(pair.distance_mm == 5000);
    assert(pair.direction_disagreement_mm == 8000);
    assert(pair.spread_mm >= pair.direction_disagreement_mm);

    /* The limit itself is still a pair; one millimetre more is not. */
    const int32_t at_limit[] = { 1000 + NRF_CS_DIRECTION_DISAGREEMENT_LIMIT_MM, 1000 + NRF_CS_DIRECTION_DISAGREEMENT_LIMIT_MM,
                                 1000 + NRF_CS_DIRECTION_DISAGREEMENT_LIMIT_MM };
    const int32_t base[] = { 1000, 1000, 1000 };
    pair = run_two_directions(3U, base, at_limit);
    assert(pair.direction_disagreement_mm == NRF_CS_DIRECTION_DISAGREEMENT_LIMIT_MM);
    assert(pair.state == NRF_CS_PAIR_VALID);
    const int32_t past_limit[] = { 1301, 1301, 1301 };
    pair = run_two_directions(3U, base, past_limit);
    assert(pair.direction_disagreement_mm == NRF_CS_DIRECTION_DISAGREEMENT_LIMIT_MM + 1);
    assert(pair.state == NRF_CS_PAIR_DIRECTIONS_DISAGREE);
}

/* The production policy takes nine samples per direction: the scheduler has to hold them and tell a wrong one from the
 * majority (two of the eighteen samples are far off and are dropped as outliers, not averaged in). */
static void test_nine_samples_per_direction_drop_the_outliers(void)
{
    const int32_t forward[] = { 3600, 3650, 3620, 3640, 3610, 9100, 3630, 3660, 3625 };
    const int32_t reverse[] = { 3700, 3680, 3690, 3710, 120, 3695, 3705, 3685, 3700 };
    const nrf_cs_pair_result_t pair = run_two_directions(9U, forward, reverse);

    assert(pair.state == NRF_CS_PAIR_VALID);
    assert(pair.valid_samples == 18U);
    assert(pair.rejected_samples == 2U);
    assert(pair.distance_mm > 3600 && pair.distance_mm < 3700);
    assert(pair.direction_disagreement_mm <= 100);
}

/* Runs pair 0 of three nodes with `forward_ok` / `reverse_ok` accepted samples in its two directions, every other
 * attempt of that direction fails until the execution gives up. */
static nrf_cs_pair_result_t run_pair_with_sample_counts_policy(
    bool full_rotation, bool both_directions, uint8_t samples, uint8_t max_attempts, size_t forward_ok, size_t reverse_ok)
{
    nrf_cs_scheduler_t scheduler;
    nrf_cs_policy_t policy = test_policy(max_attempts);
    const nrf_cs_node_id_t nodes[] = { 0U, 1U, 2U };
    const size_t wanted[2] = { forward_ok, reverse_ok };
    size_t accepted[2] = { 0U, 0U };
    nrf_cs_step_t step;

    policy.full_rotation = full_rotation;
    policy.require_both_directions = both_directions;
    policy.samples_per_pair = samples;
    assert(nrf_cs_scheduler_init(&scheduler, &policy) == NRF_CS_OK);
    assert(nrf_cs_scheduler_start_cycle(&scheduler, nodes, 3U) == NRF_CS_OK);
    while (nrf_cs_scheduler_pair_at(&scheduler, 0U)->state == NRF_CS_PAIR_PENDING) {
        assert(nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY);
        assert(step.pair_index == 0U);
        const size_t direction = step.initiator == 0U ? 0U : 1U;
        nrf_cs_measurement_t measurement = failure_measurement(NRF_CS_OUTCOME_TRANSPORT_ERROR);
        if (accepted[direction] < wanted[direction]) {
            measurement = success_measurement(2000 + (int32_t)accepted[direction], 900U);
            ++accepted[direction];
        }
        assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &measurement) == NRF_CS_OK);
    }
    return *nrf_cs_scheduler_pair_at(&scheduler, 0U);
}

static nrf_cs_pair_result_t run_pair_with_sample_counts(
    bool full_rotation, uint8_t samples, uint8_t max_attempts, size_t forward_ok, size_t reverse_ok)
{
    return run_pair_with_sample_counts_policy(full_rotation, false, samples, max_attempts, forward_ok, reverse_ok);
}

/* CS-QUALITY-04. Bench 2026-11-03: 0-1 was published (cable_exceeded, 7.77 m, spread 33 mm) and 0-3 as valid (4.39 m) from ONE
 * direction, the other having failed or ended with a single accepted sample. One direction has no second opinion; with the
 * policy bit set such a pair is not a measurement. */
static void test_a_pair_needs_both_directions_when_the_policy_says_so(void)
{
    /* Both directions with enough samples: valid either way. */
    nrf_cs_pair_result_t pair = run_pair_with_sample_counts_policy(true, true, 9U, 2U, 9U, 9U);
    assert(pair.state == NRF_CS_PAIR_VALID && !pair.single_direction && pair.valid_samples == 18U);
    pair = run_pair_with_sample_counts_policy(true, true, 9U, 2U, 3U, 3U);
    assert(pair.state == NRF_CS_PAIR_VALID && !pair.single_direction);

    /* One good direction (the 0-3 pair of the bench) and the other with nothing, or with one sample, or with two. */
    for (size_t other = 0U; other <= 2U; ++other) {
        pair = run_pair_with_sample_counts_policy(true, true, 9U, 2U, 9U, other);
        assert(pair.state == NRF_CS_PAIR_RETRY_EXHAUSTED);
        assert(pair.valid_samples == 0U && pair.distance_mm == 0 && !pair.single_direction);
        pair = run_pair_with_sample_counts_policy(true, true, 9U, 2U, other, 9U);
        assert(pair.state == NRF_CS_PAIR_RETRY_EXHAUSTED);
        /* The same data without the policy bit is still a one-sided valid pair, as before. */
        pair = run_pair_with_sample_counts_policy(true, false, 9U, 2U, 9U, other);
        assert(pair.state == NRF_CS_PAIR_VALID && pair.single_direction && pair.valid_samples == 9U);
    }

    /* Neither direction usable. */
    pair = run_pair_with_sample_counts_policy(true, true, 9U, 2U, 2U, 2U);
    assert(pair.state == NRF_CS_PAIR_RETRY_EXHAUSTED);

    /* The bit means nothing without the full rotation: a single execution per pair stays valid. */
    pair = run_pair_with_sample_counts_policy(false, true, 9U, 2U, 9U, 0U);
    assert(pair.state == NRF_CS_PAIR_VALID);
}

/* CS-QUALITY-02. Bench 2026-11-02 19:55: the 0-4 pair reported spread_mm=0 although 4 of its samples had been rejected,
 * because more than half of the samples were equal (MAD 0). A set of several samples never claims better than the floor. */
static void test_spread_never_claims_zero_for_several_samples(void)
{
    nrf_cs_scheduler_t scheduler;
    nrf_cs_policy_t policy = test_policy(1U);
    const nrf_cs_node_id_t nodes[] = { 0U, 1U, 2U };           /* three nodes: one execution per pair */
    const int32_t with_outliers[12] = { 2049, 2049, 2049, 2049, 2049, 2049, 2049, 2049, 3549, 3549, 549, 3949 };
    nrf_cs_step_t step;

    policy.samples_per_pair = 12U;
    assert(nrf_cs_scheduler_init(&scheduler, &policy) == NRF_CS_OK);
    assert(nrf_cs_scheduler_start_cycle(&scheduler, nodes, 3U) == NRF_CS_OK);
    for (size_t i = 0U; i < 12U; ++i) {
        assert(nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY);
        const nrf_cs_measurement_t measurement = success_measurement(with_outliers[i], 900U);
        assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &measurement) == NRF_CS_OK);
    }
    nrf_cs_pair_result_t pair = *nrf_cs_scheduler_pair_at(&scheduler, 0U);
    assert(pair.state == NRF_CS_PAIR_VALID);
    assert(pair.distance_mm == 2049);
    assert(pair.rejected_samples == 4U);                         /* the outliers were seen ... */
    assert(pair.spread_mm == NRF_CS_MIN_SPREAD_MM);              /* ... and the spread is not 0 */
    assert(pair.spread_mm > 0);

    /* Equal samples without outliers: still resolution-limited, not perfect. */
    const int32_t same[] = { 1000, 1000, 1000 };
    pair = run_two_directions(3U, same, same);
    assert(pair.state == NRF_CS_PAIR_VALID && pair.rejected_samples == 0U);
    assert(pair.spread_mm == NRF_CS_MIN_SPREAD_MM);

    /* One sample has no spread to speak of: that stays 0 (the legacy single-sample policy). */
    pair = run_pair_with_sample_counts(false, 1U, 1U, 1U, 0U);
    assert(pair.spread_mm == 0);

    /* The floor is a floor: a wider set keeps its own spread. MAD 100 -> sigma 148. */
    scheduler.policy.samples_per_pair = 5U;
    const int32_t wide[] = { 1000, 1100, 900, 1200, 800 };
    pair = run_two_directions(5U, wide, wide);
    assert(pair.spread_mm == 148);
}

/* CS-QUALITY-03. The 1-4 pair of the 19:55 run was published as valid from 4 samples (2 per direction) of 6 attempts. A
 * direction needs NRF_CS_MIN_SAMPLES_PER_DIRECTION accepted samples (the size whose median can drop one outlier). */
static void test_a_direction_with_too_few_samples_is_left_out(void)
{
    assert(NRF_CS_MIN_SAMPLES_PER_DIRECTION == 3U);

    /* 2 + 2 of the 1-4 pair: no usable direction at all. */
    nrf_cs_pair_result_t pair = run_pair_with_sample_counts(true, 9U, 4U, 2U, 2U);
    assert(pair.state == NRF_CS_PAIR_RETRY_EXHAUSTED);
    assert(pair.valid_samples == 0U && pair.distance_mm == 0);
    assert(!pair.single_direction);

    /* Exactly the minimum in both directions is a full pair. */
    pair = run_pair_with_sample_counts(true, 9U, 4U, 3U, 3U);
    assert(pair.state == NRF_CS_PAIR_VALID && !pair.single_direction);
    assert(pair.valid_samples == 6U);

    /* A good direction and a thin one: the thin one is left out, not averaged in, and the pair says so. */
    pair = run_pair_with_sample_counts(true, 9U, 4U, 9U, 2U);
    assert(pair.state == NRF_CS_PAIR_VALID);
    assert(pair.single_direction);
    assert(pair.valid_samples == 9U);
    assert(pair.distance_mm == 2004);                            /* median of 2000..2008 only */
    assert(pair.direction_disagreement_mm == 0);
    assert((pair.scheduler_quality_flags & NRF_CS_QUALITY_PARTIAL) != 0U);

    pair = run_pair_with_sample_counts(true, 9U, 4U, 0U, 3U);
    assert(pair.state == NRF_CS_PAIR_VALID && pair.single_direction && pair.valid_samples == 3U);

    /* Without the full rotation the one execution needs the minimum as well. */
    pair = run_pair_with_sample_counts(false, 9U, 4U, 2U, 0U);
    assert(pair.state == NRF_CS_PAIR_RETRY_EXHAUSTED);
    pair = run_pair_with_sample_counts(false, 9U, 4U, 3U, 0U);
    assert(pair.state == NRF_CS_PAIR_VALID && pair.valid_samples == 3U);

    /* A policy that asks for fewer samples than the minimum only needs what it asks for. */
    pair = run_pair_with_sample_counts(true, 2U, 2U, 2U, 2U);
    assert(pair.state == NRF_CS_PAIR_VALID && pair.valid_samples == 4U);
    pair = run_pair_with_sample_counts(true, 2U, 2U, 1U, 1U);
    assert(pair.state == NRF_CS_PAIR_RETRY_EXHAUSTED);
}

/* Feeds one direction of pair 0 a script of outcomes ('S' accepted sample, 'F' failed attempt) and returns how many
 * attempts the execution took before it ended; the other direction gets nine clean samples. */
static nrf_cs_pair_result_t run_pair_with_script(uint8_t max_attempts, const char *script, size_t *attempts_used)
{
    nrf_cs_scheduler_t scheduler;
    nrf_cs_policy_t policy = test_policy(max_attempts);
    const nrf_cs_node_id_t nodes[] = { 0U, 1U, 2U };
    nrf_cs_step_t step;
    size_t at = 0U;
    size_t other_samples = 0U;

    policy.full_rotation = true;
    policy.samples_per_pair = 9U;
    assert(nrf_cs_scheduler_init(&scheduler, &policy) == NRF_CS_OK);
    assert(nrf_cs_scheduler_start_cycle(&scheduler, nodes, 3U) == NRF_CS_OK);
    *attempts_used = 0U;
    while (nrf_cs_scheduler_pair_at(&scheduler, 0U)->state == NRF_CS_PAIR_PENDING) {
        assert(nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY);
        assert(step.pair_index == 0U);
        nrf_cs_measurement_t measurement;
        if (step.initiator == 0U) {                              /* the scripted direction */
            assert(script[at] != '\0');                          /* the script must cover every attempt that is made */
            measurement = script[at] == 'S' ? success_measurement(3000 + (int32_t)at, 900U)
                                            : failure_measurement(NRF_CS_OUTCOME_TRANSPORT_ERROR);
            ++at;
            ++*attempts_used;
        } else {
            measurement = success_measurement(3000 + (int32_t)other_samples++, 900U);
        }
        assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &measurement) == NRF_CS_OK);
    }
    return *nrf_cs_scheduler_pair_at(&scheduler, 0U);
}

/* Bench 2026-11-02: with max_attempts 1 the first TRANSPORT_ERROR (status 9 in SECURING, 4 of 24 attempts) ended the
 * whole direction; 0->1 stopped after 2 samples, 0->4 after 5. With a retry budget a failure costs one attempt and the
 * series goes on until the failures reach max_attempts or the nine samples are in. */
static void test_a_failed_attempt_ends_the_series_only_when_the_retry_budget_is_spent(void)
{
    size_t used;
    nrf_cs_pair_result_t pair;

    /* No retry budget (production policy today): S S F ends the direction with 2 samples; 2 < 3, the direction is out. */
    pair = run_pair_with_script(1U, "SSF", &used);
    assert(used == 3U);
    assert(pair.state == NRF_CS_PAIR_VALID && pair.single_direction && pair.valid_samples == 9U);   /* the clean direction */

    /* Four tries: three failures are survived, the fourth ends it. */
    pair = run_pair_with_script(4U, "SSFSSFSSFSSS", &used);
    assert(used == 12U);                                        /* 9 samples + 3 failures = the 12 attempts the plan allows */
    assert(pair.state == NRF_CS_PAIR_VALID && !pair.single_direction && pair.valid_samples == 18U);

    pair = run_pair_with_script(4U, "SSFSFSFSSSSSS", &used);
    assert(used == 12U);                                        /* three failures in a row of tries: the 4th is never reached */
    assert(pair.state == NRF_CS_PAIR_VALID && !pair.single_direction && pair.valid_samples == 18U);
    pair = run_pair_with_script(4U, "SSSFFFF", &used);
    assert(used == 7U);                                         /* four failures end it with the 3 samples it has */
    assert(pair.state == NRF_CS_PAIR_VALID && !pair.single_direction && pair.valid_samples == 12U);   /* 3 + 9 */

    /* Two failures too many leave 2 samples: below the minimum, that direction is dropped. */
    pair = run_pair_with_script(2U, "SSFF", &used);
    assert(used == 4U);
    assert(pair.state == NRF_CS_PAIR_VALID && pair.single_direction && pair.valid_samples == 9U);

    /* The retry budget never lets a series exceed the samples asked for. */
    pair = run_pair_with_script(4U, "SSSSSSSSSS", &used);
    assert(used == 9U);
    assert(pair.valid_samples == 18U);
}

/* Runs a whole cycle and records the directed executions in the order the scheduler hands them out. */
static size_t relay_order_of(
    const nrf_cs_node_id_t *nodes, size_t node_count, bool relay, nrf_cs_node_id_t *initiators,
    nrf_cs_node_id_t *reflectors, uint8_t *pair_indexes, nrf_cs_pair_result_t *pairs_out)
{
    nrf_cs_scheduler_t scheduler;
    nrf_cs_policy_t policy = test_policy(1U);
    nrf_cs_step_t step;
    size_t count = 0U;

    policy.full_rotation = true;
    policy.relay_order = relay;
    assert(nrf_cs_scheduler_init(&scheduler, &policy) == NRF_CS_OK);
    assert(nrf_cs_scheduler_start_cycle(&scheduler, nodes, node_count) == NRF_CS_OK);
    while (nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY) {
        initiators[count] = step.initiator;
        reflectors[count] = step.reflector;
        pair_indexes[count] = step.pair_index;
        ++count;
        const nrf_cs_measurement_t measurement = success_measurement(1000, 900U);
        assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &measurement) == NRF_CS_OK);
    }
    assert(nrf_cs_scheduler_is_complete(&scheduler));
    for (size_t p = 0U; p < nrf_cs_scheduler_pair_count(&scheduler); ++p) pairs_out[p] = *nrf_cs_scheduler_pair_at(&scheduler, p);
    return count;
}

/* Relay order: the reflector of one execution is the initiator of the next, so the node that has just been measured can
 * start at once when the initiator lets go. An Eulerian circuit of the complete directed graph does that for any size. */
static void test_relay_order_chains_every_execution_to_the_next(void)
{
    static const nrf_cs_node_id_t sets[4][5] = {
        { 3U, 0U }, { 9U, 0U, 4U }, { 7U, 2U, 0U, 5U }, { 4U, 0U, 6U, 1U, 3U },
    };
    static const size_t counts[4] = { 2U, 3U, 4U, 5U };

    for (size_t s = 0U; s < 4U; ++s) {
        nrf_cs_node_id_t initiators[NRF_CS_MAX_EXECUTIONS], reflectors[NRF_CS_MAX_EXECUTIONS];
        uint8_t pair_indexes[NRF_CS_MAX_EXECUTIONS];
        nrf_cs_pair_result_t pairs[NRF_CS_MAX_PAIRS];
        const size_t n = counts[s];
        const size_t executions = relay_order_of(sets[s], n, true, initiators, reflectors, pair_indexes, pairs);

        assert(executions == n * (n - 1U));
        /* Every ordered pair of nodes exactly once. */
        for (size_t a = 0U; a < n; ++a) {
            for (size_t b = 0U; b < n; ++b) {
                if (a == b) continue;
                unsigned seen = 0U;
                for (size_t e = 0U; e < executions; ++e) {
                    if (initiators[e] == sets[s][a] && reflectors[e] == sets[s][b]) ++seen;
                }
                assert(seen == 1U);
            }
        }
        /* The chain, and the circuit closes at the node it starts at: the smallest id, MAIN (0). */
        for (size_t e = 0U; e + 1U < executions; ++e) assert(reflectors[e] == initiators[e + 1U]);
        assert(initiators[0] == 0U && reflectors[executions - 1U] == 0U);
        /* The pair index is the pair of the two nodes, whichever way they are ordered. */
        for (size_t e = 0U; e < executions; ++e) {
            const nrf_cs_pair_result_t *pair = &pairs[pair_indexes[e]];
            assert((pair->node_a == initiators[e] && pair->node_b == reflectors[e]) ||
                   (pair->node_a == reflectors[e] && pair->node_b == initiators[e]));
        }
        /* Every pair got both directions, so each is complete. */
        for (size_t p = 0U; p < n * (n - 1U) / 2U; ++p) {
            assert(pairs[p].state == NRF_CS_PAIR_VALID && !pairs[p].single_direction && pairs[p].valid_samples == 2U);
        }
        /* The order depends on the node set, not on the order the nodes were given in. */
        nrf_cs_node_id_t shuffled[5];
        for (size_t i = 0U; i < n; ++i) shuffled[i] = sets[s][n - 1U - i];
        nrf_cs_node_id_t i2[NRF_CS_MAX_EXECUTIONS], r2[NRF_CS_MAX_EXECUTIONS];
        uint8_t p2[NRF_CS_MAX_EXECUTIONS];
        nrf_cs_pair_result_t pairs2[NRF_CS_MAX_PAIRS];
        assert(relay_order_of(shuffled, n, true, i2, r2, p2, pairs2) == executions);
        assert(memcmp(initiators, i2, executions) == 0 && memcmp(reflectors, r2, executions) == 0);
        /* Each node initiates n - 1 times: nobody is measured more than the others. */
        for (size_t a = 0U; a < n; ++a) {
            unsigned initiated = 0U;
            for (size_t e = 0U; e < executions; ++e) if (initiators[e] == sets[s][a]) ++initiated;
            assert(initiated == n - 1U);
        }
    }

    /* The three-node circuit written out: 0->1, 1->0 is not a chain start of a circuit; the algorithm gives this order. */
    nrf_cs_node_id_t i3[6], r3[6];
    uint8_t p3[6];
    nrf_cs_pair_result_t pr3[3];
    const nrf_cs_node_id_t three[] = { 0U, 1U, 2U };
    assert(relay_order_of(three, 3U, true, i3, r3, p3, pr3) == 6U);
    const nrf_cs_node_id_t expect_i[6] = { 0U, 1U, 0U, 2U, 1U, 2U };
    const nrf_cs_node_id_t expect_r[6] = { 1U, 0U, 2U, 1U, 2U, 0U };
    assert(memcmp(i3, expect_i, 6U) == 0 && memcmp(r3, expect_r, 6U) == 0);

    /* Without the flag, or without the full rotation, the order stays as it was. */
    nrf_cs_node_id_t ip[6], rp[6];
    uint8_t pp[6];
    nrf_cs_pair_result_t pr[3];
    assert(relay_order_of(three, 3U, false, ip, rp, pp, pr) == 6U);
    const nrf_cs_node_id_t pairwise_i[6] = { 0U, 1U, 0U, 2U, 1U, 2U };
    const nrf_cs_node_id_t pairwise_r[6] = { 1U, 0U, 2U, 0U, 2U, 1U };
    assert(memcmp(ip, pairwise_i, 6U) == 0 && memcmp(rp, pairwise_r, 6U) == 0);
}

static void test_abort_cycle_completes_pending_executions_with_timeout(void)
{
    nrf_cs_scheduler_t scheduler;
    nrf_cs_policy_t policy = test_policy(1U);
    const nrf_cs_node_id_t nodes[] = { 0U, 1U, 2U };
    nrf_cs_step_t step;
    const nrf_cs_measurement_t timeout = failure_measurement(NRF_CS_OUTCOME_TIMEOUT);

    policy.full_rotation = true;
    assert(nrf_cs_scheduler_init(&scheduler, &policy) == NRF_CS_OK);
    assert(nrf_cs_scheduler_start_cycle(&scheduler, nodes, 3U) == NRF_CS_OK);
    assert(nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY);
    assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &timeout) == NRF_CS_OK);
    assert(nrf_cs_scheduler_abort_cycle(&scheduler, NRF_CS_FAILURE_TIMEOUT) == NRF_CS_OK);
    assert(nrf_cs_scheduler_is_complete(&scheduler));
    assert(nrf_cs_scheduler_executions_done(&scheduler) == 6U);
    for (size_t i = 0U; i < nrf_cs_scheduler_pair_count(&scheduler); ++i) {
        const nrf_cs_pair_result_t *pair = nrf_cs_scheduler_pair_at(&scheduler, i);
        assert(pair != NULL);
        if (i == 0U) {
            assert(pair->state == NRF_CS_PAIR_RETRY_EXHAUSTED);
            assert(pair->last_failure == NRF_CS_FAILURE_TIMEOUT);
        } else {
            assert(pair->state == NRF_CS_PAIR_RETRY_EXHAUSTED);
            assert(pair->scheduler_quality_flags & NRF_CS_QUALITY_TIMEOUT);
        }
    }
}


/* Bench 2026-11-03: MAIN-G4 ended RETRY_EXHAUSTED and the measured numbers were gone. Every state keeps what each direction produced,
 * and the cause of a missing distance is told apart: no sample at all, too few above min_confidence, too few of any kind, contradicting
 * directions, a node that left. */
typedef struct {
    nrf_cs_outcome_t outcome;
    int32_t distance_mm;
    uint16_t confidence;
} scripted_attempt_t;

static nrf_cs_pair_result_t run_scripted_pair(const scripted_attempt_t *ab, size_t ab_count,
    const scripted_attempt_t *ba, size_t ba_count, bool disconnect_before_reverse)
{
    nrf_cs_scheduler_t scheduler;
    nrf_cs_policy_t policy = test_policy(2U);
    const nrf_cs_node_id_t nodes[] = { 1U, 2U };
    nrf_cs_step_t step;
    size_t used_ab = 0U, used_ba = 0U;

    policy.min_confidence = 500U;
    policy.samples_per_pair = 9U;
    policy.full_rotation = true;
    policy.require_both_directions = true;
    assert(nrf_cs_scheduler_init(&scheduler, &policy) == NRF_CS_OK);
    assert(nrf_cs_scheduler_start_cycle(&scheduler, nodes, 2U) == NRF_CS_OK);
    while (nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY) {
        const bool forward = step.initiator == 1U;
        const scripted_attempt_t *list = forward ? ab : ba;
        size_t *used = forward ? &used_ab : &used_ba;
        nrf_cs_measurement_t measurement = failure_measurement(NRF_CS_OUTCOME_TIMEOUT);

        if (!forward && disconnect_before_reverse) {
            assert(nrf_cs_scheduler_disconnect(&scheduler, 2U) == NRF_CS_OK);
            break;
        }
        if (*used < (forward ? ab_count : ba_count)) {
            measurement.outcome = list[*used].outcome;
            measurement.distance_mm = list[*used].distance_mm;
            measurement.confidence = list[*used].confidence;
            ++*used;
        }
        assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &measurement) == NRF_CS_OK);
    }
    return *nrf_cs_scheduler_pair_at(&scheduler, 0U);
}

static void test_a_pair_without_a_distance_keeps_the_numbers_and_the_cause(void)
{
    scripted_attempt_t good[9], contradicting[9], rejected[3], two_then_nothing[2], errors[2];
    for (size_t i = 0U; i < 9U; ++i) {
        good[i] = (scripted_attempt_t){ NRF_CS_OUTCOME_SUCCESS, 4000 + (int32_t)i * 4, 900U };
        contradicting[i] = (scripted_attempt_t){ NRF_CS_OUTCOME_SUCCESS, 6400 + (int32_t)i * 4, 900U };
    }
    rejected[0] = (scripted_attempt_t){ NRF_CS_OUTCOME_SUCCESS, 8845, 902U };
    rejected[1] = (scripted_attempt_t){ NRF_CS_OUTCOME_SUCCESS, 8811, 492U };
    rejected[2] = (scripted_attempt_t){ NRF_CS_OUTCOME_SUCCESS, 8857, 358U };
    two_then_nothing[0] = (scripted_attempt_t){ NRF_CS_OUTCOME_SUCCESS, 4100, 900U };
    two_then_nothing[1] = (scripted_attempt_t){ NRF_CS_OUTCOME_SUCCESS, 4110, 900U };
    errors[0] = errors[1] = (scripted_attempt_t){ NRF_CS_OUTCOME_TRANSPORT_ERROR, 0, 0U };

    /* A valid pair: both directions complete, nothing to explain, the medians are there. */
    nrf_cs_pair_result_t pair = run_scripted_pair(good, 9U, good, 9U, false);
    assert(pair.state == NRF_CS_PAIR_VALID && nrf_cs_pair_cause(&pair, 3U) == NRF_CS_PAIR_CAUSE_NONE);
    assert(pair.diag_distance_mm[0] == 4016 && pair.diag_distance_mm[1] == 4016);
    assert(pair.diag_samples[0] == 9U && pair.diag_accepted[0] == 9U && pair.diag_samples[1] == 9U && pair.diag_accepted[1] == 9U);

    /* The bench case: one direction produced three successes of which one met min_confidence, the other direction is complete. */
    pair = run_scripted_pair(rejected, 3U, good, 9U, false);
    assert(pair.state == NRF_CS_PAIR_RETRY_EXHAUSTED && pair.distance_mm == 0 && pair.valid_samples == 0U);
    assert(pair.diag_distance_mm[0] == 8845 && pair.diag_samples[0] == 3U && pair.diag_accepted[0] == 1U);   /* median of all three */
    assert(pair.diag_distance_mm[1] == 4016 && pair.diag_samples[1] == 9U && pair.diag_accepted[1] == 9U);
    assert(nrf_cs_pair_cause(&pair, 3U) == NRF_CS_PAIR_CAUSE_LOW_CONFIDENCE);
    assert(strcmp(nrf_cs_pair_cause_name(nrf_cs_pair_cause(&pair, 3U)), "low_confidence") == 0);

    /* Nothing came back at all in one direction. */
    pair = run_scripted_pair(errors, 2U, good, 9U, false);
    assert(pair.state == NRF_CS_PAIR_RETRY_EXHAUSTED && pair.diag_samples[0] == 0U && pair.diag_distance_mm[0] == 0);
    assert(nrf_cs_pair_cause(&pair, 3U) == NRF_CS_PAIR_CAUSE_NO_MEASUREMENT && pair.diag_samples[1] == 9U);

    /* Two good samples and then nothing: fewer than the three needed, nothing rejected for confidence. */
    pair = run_scripted_pair(two_then_nothing, 2U, good, 9U, false);
    assert(pair.state == NRF_CS_PAIR_RETRY_EXHAUSTED && pair.diag_samples[0] == 2U && pair.diag_accepted[0] == 2U);
    assert(nrf_cs_pair_cause(&pair, 3U) == NRF_CS_PAIR_CAUSE_FEW_SAMPLES);

    /* Both directions complete and contradicting: the state says so, the numbers stay in the pair and in the diagnostics. */
    pair = run_scripted_pair(good, 9U, contradicting, 9U, false);
    assert(pair.state == NRF_CS_PAIR_DIRECTIONS_DISAGREE && pair.direction_disagreement_mm > NRF_CS_DIRECTION_DISAGREEMENT_LIMIT_MM);
    assert(nrf_cs_pair_cause(&pair, 3U) == NRF_CS_PAIR_CAUSE_DIRECTIONS_DISAGREE);
    assert(pair.diag_distance_mm[0] == 4016 && pair.diag_distance_mm[1] == 6416);

    /* A node left between the directions. */
    pair = run_scripted_pair(good, 9U, good, 9U, true);
    assert(pair.state == NRF_CS_PAIR_MISSING && nrf_cs_pair_cause(&pair, 3U) == NRF_CS_PAIR_CAUSE_DISCONNECTED);
    assert(pair.diag_samples[0] == 9U && pair.diag_samples[1] == 0U);

    /* A nothing-yet pair, a missing argument and the names. */
    nrf_cs_pair_result_t empty;
    memset(&empty, 0, sizeof(empty));
    empty.state = NRF_CS_PAIR_PENDING;
    assert(nrf_cs_pair_cause(&empty, 3U) == NRF_CS_PAIR_CAUSE_NONE && nrf_cs_pair_cause(NULL, 3U) == NRF_CS_PAIR_CAUSE_NONE);
    assert(strcmp(nrf_cs_pair_cause_name(NRF_CS_PAIR_CAUSE_CABLE_EXCEEDED), "cable_exceeded") == 0);
    assert(strcmp(nrf_cs_pair_cause_name((nrf_cs_pair_cause_t)99), "none") == 0);
}

static void test_the_pair_summary_tells_failed_pairs_from_no_measurement_at_all(void)
{
    nrf_cs_pair_result_t pairs[4];
    char text[96];

    memset(pairs, 0, sizeof(pairs));
    for (size_t i = 0U; i < 4U; ++i) {
        pairs[i].node_a = 0U;
        pairs[i].node_b = (nrf_cs_node_id_t)(i + 1U);
        pairs[i].state = NRF_CS_PAIR_VALID;
    }
    assert(nrf_cs_pairs_summary(pairs, 4U, 3U, text, sizeof(text)) == 4U && text[0] == 0);       /* all valid: nothing failed */

    pairs[3].state = NRF_CS_PAIR_RETRY_EXHAUSTED;
    pairs[3].diag_samples[0] = 3U; pairs[3].diag_accepted[0] = 1U; pairs[3].diag_samples[1] = 9U; pairs[3].diag_accepted[1] = 9U;
    pairs[1].state = NRF_CS_PAIR_DIRECTIONS_DISAGREE;
    assert(nrf_cs_pairs_summary(pairs, 4U, 3U, text, sizeof(text)) == 2U);
    assert(strcmp(text, "0-2:directions_disagree,0-4:low_confidence") == 0);

    for (size_t i = 0U; i < 4U; ++i) {
        pairs[i].state = NRF_CS_PAIR_RETRY_EXHAUSTED;
        memset(pairs[i].diag_samples, 0, sizeof(pairs[i].diag_samples));
    }
    assert(nrf_cs_pairs_summary(pairs, 4U, 3U, text, sizeof(text)) == 0U);                         /* no measurement at all */
    assert(strstr(text, "0-1:no_measurement") == text && strstr(text, "0-4:no_measurement") != NULL);

    char tiny[8];
    assert(nrf_cs_pairs_summary(pairs, 4U, 3U, tiny, sizeof(tiny)) == 0U && strcmp(tiny, "...") == 0);
    assert(nrf_cs_pairs_summary(NULL, 4U, 3U, text, sizeof(text)) == 0U && text[0] == 0);
    assert(nrf_cs_pairs_summary(pairs, 0U, 3U, NULL, 0U) == 0U);
    pairs[0].state = NRF_CS_PAIR_PENDING;                                                           /* not decided yet: not a failure */
    assert(nrf_cs_pairs_summary(pairs, 1U, 3U, text, sizeof(text)) == 0U && text[0] == 0);
}

int main(void)
{
    test_a_pair_without_a_distance_keeps_the_numbers_and_the_cause();
    test_the_pair_summary_tells_failed_pairs_from_no_measurement_at_all();
    test_multi_sample_median_rejects_outlier();
    test_single_sample_policy_keeps_legacy_behaviour();
    test_samples_per_pair_is_bounded();
    test_pair_enumeration_is_canonical_and_discovery_order_independent();
    test_initiator_fairness_for_three_four_and_five_nodes();
    test_two_nodes_get_reciprocal_directed_steps_for_one_matrix_pair();
    test_one_active_step_and_stale_attempt_tokens();
    test_timeout_retry_exhaustion_is_terminal_and_explicit();
    test_low_confidence_success_retries_as_outlier();
    test_disconnect_cancels_only_affected_unfinished_pairs_and_rejoin_is_next_cycle();
    test_main_identity_has_no_privileged_initiator_role();
    test_empty_and_single_node_cycles_complete_without_steps();
    test_invalid_inputs_are_rejected();
    test_full_rotation_has_both_orders_in_canonical_sequence();
    test_full_rotation_pair_combine_and_direction_failures();
    test_contradicting_directions_are_not_a_valid_pair();
    test_nine_samples_per_direction_drop_the_outliers();
    test_spread_never_claims_zero_for_several_samples();
    test_a_direction_with_too_few_samples_is_left_out();
    test_a_failed_attempt_ends_the_series_only_when_the_retry_budget_is_spent();
    test_relay_order_chains_every_execution_to_the_next();
    test_a_pair_needs_both_directions_when_the_policy_says_so();
    test_abort_cycle_completes_pending_executions_with_timeout();
    return 0;
}
