#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "nrf_cs_scheduler.h"

static nrf_cs_policy_t test_policy(uint8_t max_attempts)
{
    nrf_cs_policy_t policy;
    policy.pair_timeout_ms = 120U;
    policy.max_attempts = max_attempts;
    policy.min_confidence = 600U;
    policy.samples_per_pair = 1U;
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


int main(void)
{
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
    return 0;
}
