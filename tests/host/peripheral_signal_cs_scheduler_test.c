/*
 * Channel Sounding cycle scheduler: policy domain, membership/topology for
 * 0..5 nodes, every outcome x confidence combination, retry budget, outlier
 * statistics (including extreme distances), disconnect and abort.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nrf_cs_scheduler.h"

#define CASE_PASS(name) printf("PERIPHERAL_CASE %s PASS\n", name)

static nrf_cs_policy_t policy(uint8_t samples, uint8_t attempts, uint16_t min_confidence, bool rotation)
{
    nrf_cs_policy_t value;
    memset(&value, 0, sizeof(value));
    value.pair_timeout_ms = 500U;
    value.max_attempts = attempts;
    value.min_confidence = min_confidence;
    value.samples_per_pair = samples;
    value.full_rotation = rotation;
    return value;
}

static nrf_cs_measurement_t measurement(nrf_cs_outcome_t outcome, int32_t distance, uint16_t confidence)
{
    nrf_cs_measurement_t value;
    memset(&value, 0, sizeof(value));
    value.outcome = outcome;
    value.distance_mm = distance;
    value.confidence = confidence;
    return value;
}

static void start_nodes(nrf_cs_scheduler_t *scheduler, const nrf_cs_policy_t *p, size_t count)
{
    static const nrf_cs_node_id_t ids[NRF_CS_MAX_NODES] = {0U, 1U, 2U, 3U, 4U};
    assert(nrf_cs_scheduler_init(scheduler, p) == NRF_CS_OK);
    assert(nrf_cs_scheduler_start_cycle(scheduler, ids, count) == NRF_CS_OK);
}

static void policy_domain_is_enforced(void)
{
    nrf_cs_scheduler_t scheduler;
    nrf_cs_policy_t p = policy(1U, 1U, 0U, false);

    assert(nrf_cs_scheduler_init(NULL, &p) == NRF_CS_ERR_ARGUMENT);
    assert(nrf_cs_scheduler_init(&scheduler, NULL) == NRF_CS_ERR_ARGUMENT);
    p = policy(1U, 1U, 0U, false);
    p.pair_timeout_ms = 0U;
    assert(nrf_cs_scheduler_init(&scheduler, &p) == NRF_CS_ERR_ARGUMENT);
    p = policy(1U, 0U, 0U, false);
    assert(nrf_cs_scheduler_init(&scheduler, &p) == NRF_CS_ERR_ARGUMENT);
    for (unsigned confidence = 0U; confidence <= NRF_CS_CONFIDENCE_MAX + 3U; ++confidence) {
        p = policy(1U, 1U, (uint16_t)confidence, false);
        assert((nrf_cs_scheduler_init(&scheduler, &p) == NRF_CS_OK) == (confidence <= NRF_CS_CONFIDENCE_MAX));
    }
    for (unsigned samples = 0U; samples <= 255U; ++samples) {
        p = policy((uint8_t)samples, 1U, 0U, false);
        assert((nrf_cs_scheduler_init(&scheduler, &p) == NRF_CS_OK) ==
               (samples <= NRF_CS_MAX_SAMPLES_PER_EXECUTION));
    }
    CASE_PASS("CS.SCHEDULER.policy_domain");
}

static void membership_and_topology_for_zero_to_five_nodes(void)
{
    static const size_t pairs_for[] = {0U, 0U, 1U, 3U, 6U, 10U};

    for (size_t nodes = 0U; nodes <= NRF_CS_MAX_NODES; ++nodes) {
        for (int rotation = 0; rotation <= 1; ++rotation) {
            nrf_cs_scheduler_t scheduler;
            const nrf_cs_policy_t p = policy(1U, 1U, 0U, rotation != 0);
            start_nodes(&scheduler, &p, nodes);
            assert(nrf_cs_scheduler_pair_count(&scheduler) == pairs_for[nodes]);
            size_t executions = pairs_for[nodes];
            if (rotation != 0 || nodes == 2U) executions *= 2U;
            assert(nrf_cs_scheduler_execution_count(&scheduler) == executions);
            assert(nrf_cs_scheduler_is_complete(&scheduler) == (executions == 0U));
            for (size_t i = 0U; i < pairs_for[nodes]; ++i) {
                const nrf_cs_pair_result_t *pair = nrf_cs_scheduler_pair_at(&scheduler, i);
                assert(pair != NULL && pair->state == NRF_CS_PAIR_PENDING && pair->node_a < pair->node_b);
                assert(nrf_cs_scheduler_matrix_cell(&scheduler, pair->node_b, pair->node_a) == pair);
            }
            assert(nrf_cs_scheduler_pair_at(&scheduler, pairs_for[nodes]) == NULL);
        }
    }

    /* Invalid membership. */
    {
        nrf_cs_scheduler_t scheduler;
        const nrf_cs_policy_t p = policy(1U, 1U, 0U, false);
        const nrf_cs_node_id_t duplicate[] = {1U, 2U, 1U};
        const nrf_cs_node_id_t six[] = {0U, 1U, 2U, 3U, 4U, 5U};
        assert(nrf_cs_scheduler_init(&scheduler, &p) == NRF_CS_OK);
        assert(nrf_cs_scheduler_start_cycle(&scheduler, duplicate, 3U) == NRF_CS_ERR_ARGUMENT);
        assert(nrf_cs_scheduler_start_cycle(&scheduler, six, 6U) == NRF_CS_ERR_ARGUMENT);
        assert(nrf_cs_scheduler_start_cycle(&scheduler, NULL, 2U) == NRF_CS_ERR_ARGUMENT);
        assert(nrf_cs_scheduler_start_cycle(NULL, six, 2U) == NRF_CS_ERR_ARGUMENT);
        assert(!nrf_cs_scheduler_is_complete(&scheduler));
        assert(nrf_cs_scheduler_matrix_cell(&scheduler, 1U, 1U) == NULL);
    }
    CASE_PASS("CS.SCHEDULER.topology");
}

static void every_pair_runs_exactly_once_with_unique_nonzero_attempt_ids(void)
{
    for (size_t nodes = 3U; nodes <= NRF_CS_MAX_NODES; ++nodes) {
        nrf_cs_scheduler_t scheduler;
        const nrf_cs_policy_t p = policy(1U, 1U, 0U, false);
        unsigned seen[NRF_CS_MAX_PAIRS] = {0};
        uint32_t attempt_ids[NRF_CS_MAX_EXECUTIONS];
        size_t steps = 0U;
        nrf_cs_step_t step;

        start_nodes(&scheduler, &p, nodes);
        while (nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY) {
            const nrf_cs_pair_result_t *pair = nrf_cs_scheduler_pair_at(&scheduler, step.pair_index);
            assert(pair != NULL);
            assert((step.initiator == pair->node_a && step.reflector == pair->node_b) ||
                   (step.initiator == pair->node_b && step.reflector == pair->node_a));
            assert(step.timeout_ms == 500U && step.attempt_number == 1U && step.attempt_id != 0U);
            assert(step.cycle_id != 0U && step.membership_generation != 0U);
            for (size_t i = 0U; i < steps; ++i) assert(attempt_ids[i] != step.attempt_id);
            attempt_ids[steps++] = step.attempt_id;
            ++seen[step.pair_index];
            /* The scheduler hands out one step at a time. */
            nrf_cs_step_t other;
            assert(nrf_cs_scheduler_next_step(&scheduler, &other) == NRF_CS_ERR_BUSY);
            assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &(nrf_cs_measurement_t){
                .outcome = NRF_CS_OUTCOME_SUCCESS, .distance_mm = 1000, .confidence = 900U}) == NRF_CS_OK);
        }
        assert(nrf_cs_scheduler_is_complete(&scheduler));
        assert(steps == nodes * (nodes - 1U) / 2U);
        for (size_t i = 0U; i < steps; ++i) assert(seen[i] == 1U);
    }
    CASE_PASS("CS.SCHEDULER.step_stream");
}

typedef struct {
    nrf_cs_outcome_t outcome;
    uint16_t confidence;
    bool counted_as_sample;
    nrf_cs_failure_reason_t failure;
    uint16_t quality_flag;
} outcome_row_t;

static void every_outcome_and_confidence_has_one_defined_effect(void)
{
    const uint16_t min_confidence = 500U;
    const uint16_t confidences[] = {0U, 499U, 500U, 501U, NRF_CS_CONFIDENCE_MAX};
    const outcome_row_t rows[] = {
        {NRF_CS_OUTCOME_SUCCESS, 0U, false, NRF_CS_FAILURE_OUTLIER, NRF_CS_QUALITY_LOW_CONFIDENCE},
        {NRF_CS_OUTCOME_TIMEOUT, 0U, false, NRF_CS_FAILURE_TIMEOUT, NRF_CS_QUALITY_TIMEOUT},
        {NRF_CS_OUTCOME_TRANSPORT_ERROR, 0U, false, NRF_CS_FAILURE_TRANSPORT, NRF_CS_QUALITY_TRANSPORT_ERROR},
        {NRF_CS_OUTCOME_OUTLIER, 0U, false, NRF_CS_FAILURE_OUTLIER, NRF_CS_QUALITY_OUTLIER},
    };

    for (size_t r = 0U; r < sizeof(rows) / sizeof(rows[0]); ++r) {
        for (size_t c = 0U; c < sizeof(confidences) / sizeof(confidences[0]); ++c) {
            nrf_cs_scheduler_t scheduler;
            const nrf_cs_policy_t p = policy(1U, 1U, min_confidence, false);
            nrf_cs_step_t step;
            start_nodes(&scheduler, &p, 3U);
            assert(nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY);
            const nrf_cs_measurement_t m = measurement(rows[r].outcome, 1234, confidences[c]);
            assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &m) == NRF_CS_OK);

            const nrf_cs_pair_result_t *pair = nrf_cs_scheduler_pair_at(&scheduler, step.pair_index);
            const bool accepted = rows[r].outcome == NRF_CS_OUTCOME_SUCCESS && confidences[c] >= min_confidence;
            if (accepted) {
                assert(pair->state == NRF_CS_PAIR_VALID && pair->distance_mm == 1234);
                assert(pair->valid_samples == 1U && pair->last_failure == NRF_CS_FAILURE_NONE);
                assert(pair->confidence == confidences[c]);
            } else {
                /* max_attempts is 1, so a single failure exhausts the budget. */
                assert(pair->state == NRF_CS_PAIR_RETRY_EXHAUSTED);
                assert(pair->distance_mm == 0 && pair->valid_samples == 0U && pair->confidence == 0U);
                assert(pair->last_failure == rows[r].failure);
                assert((pair->scheduler_quality_flags & rows[r].quality_flag) != 0U);
            }
        }
    }

    /* Domain violations are refused without touching the pair. */
    {
        nrf_cs_scheduler_t scheduler;
        const nrf_cs_policy_t p = policy(1U, 3U, 0U, false);
        nrf_cs_step_t step;
        start_nodes(&scheduler, &p, 3U);
        assert(nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY);
        nrf_cs_measurement_t bad = measurement(NRF_CS_OUTCOME_SUCCESS, -1, 900U);
        assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &bad) == NRF_CS_ERR_ARGUMENT);
        bad = measurement(NRF_CS_OUTCOME_SUCCESS, 10, NRF_CS_CONFIDENCE_MAX + 1U);
        assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &bad) == NRF_CS_ERR_ARGUMENT);
        for (unsigned outcome = NRF_CS_OUTCOME_OUTLIER + 1U; outcome < 300U; ++outcome) {
            bad = measurement((nrf_cs_outcome_t)outcome, 10, 900U);
            assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &bad) == NRF_CS_ERR_ARGUMENT);
        }
        assert(nrf_cs_scheduler_complete_step(&scheduler, NULL, &bad) == NRF_CS_ERR_ARGUMENT);
        assert(nrf_cs_scheduler_complete_step(&scheduler, &step, NULL) == NRF_CS_ERR_ARGUMENT);
        /* The step is still the active one and can complete normally. */
        const nrf_cs_measurement_t good = measurement(NRF_CS_OUTCOME_SUCCESS, 500, 900U);
        assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &good) == NRF_CS_OK);
        /* A negative distance is meaningless only for a success. */
        assert(nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY);
        const nrf_cs_measurement_t failed = measurement(NRF_CS_OUTCOME_TIMEOUT, -7, 0U);
        assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &failed) == NRF_CS_OK);
    }
    CASE_PASS("CS.SCHEDULER.outcome_confidence_matrix");
}

static void stale_steps_are_rejected_for_every_identity_field(void)
{
    for (int field = 0; field < 7; ++field) {
        nrf_cs_scheduler_t scheduler;
        const nrf_cs_policy_t p = policy(1U, 2U, 0U, false);
        nrf_cs_step_t step;
        start_nodes(&scheduler, &p, 3U);
        assert(nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY);
        nrf_cs_step_t other = step;
        switch (field) {
        case 0: other.cycle_id += 1U; break;
        case 1: other.membership_generation += 1U; break;
        case 2: other.attempt_id += 1U; break;
        case 3: other.pair_index += 1U; break;
        case 4: other.attempt_number += 1U; break;
        case 5: other.initiator += 1U; break;
        default: other.reflector += 1U; break;
        }
        const nrf_cs_measurement_t ok = measurement(NRF_CS_OUTCOME_SUCCESS, 1000, 900U);
        assert(nrf_cs_scheduler_complete_step(&scheduler, &other, &ok) == NRF_CS_ERR_STALE);
        assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &ok) == NRF_CS_OK);
        /* Completing a step twice is stale as well. */
        assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &ok) == NRF_CS_ERR_STALE);
    }
    CASE_PASS("CS.SCHEDULER.stale_identity");
}

static void only_failed_attempts_consume_the_retry_budget(void)
{
    /* 3 samples wanted, 2 failures allowed: S F S F S -> valid with 3 samples. */
    nrf_cs_scheduler_t scheduler;
    const nrf_cs_policy_t p = policy(3U, 2U, 0U, false);
    const nrf_cs_policy_t generous = policy(3U, 3U, 0U, false);
    nrf_cs_step_t step;
    const nrf_cs_outcome_t script[] = {
        NRF_CS_OUTCOME_SUCCESS, NRF_CS_OUTCOME_TIMEOUT, NRF_CS_OUTCOME_SUCCESS,
        NRF_CS_OUTCOME_TRANSPORT_ERROR, NRF_CS_OUTCOME_SUCCESS,
    };

    start_nodes(&scheduler, &generous, 2U);
    /* Two nodes run both directions; drive only the first execution to the end. */
    for (size_t i = 0U; i < sizeof(script) / sizeof(script[0]); ++i) {
        assert(nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY);
        assert(step.attempt_number == (uint8_t)(i + 1U));
        const nrf_cs_measurement_t m = measurement(script[i], 1000 + (int32_t)i, 900U);
        assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &m) == NRF_CS_OK);
    }
    assert(nrf_cs_scheduler_executions_done(&scheduler) == 1U);

    /* A third failure on the second execution exhausts it but keeps what was collected. */
    nrf_cs_scheduler_t second;
    start_nodes(&second, &p, 3U);
    assert(nrf_cs_scheduler_next_step(&second, &step) == NRF_CS_STEP_READY);
    assert(nrf_cs_scheduler_complete_step(&second, &step, &(nrf_cs_measurement_t){
        .outcome = NRF_CS_OUTCOME_SUCCESS, .distance_mm = 2000, .confidence = 900U}) == NRF_CS_OK);
    for (int failure = 0; failure < 2; ++failure) {
        assert(nrf_cs_scheduler_next_step(&second, &step) == NRF_CS_STEP_READY);
        assert(nrf_cs_scheduler_complete_step(&second, &step, &(nrf_cs_measurement_t){
            .outcome = NRF_CS_OUTCOME_TIMEOUT}) == NRF_CS_OK);
    }
    const nrf_cs_pair_result_t *pair = nrf_cs_scheduler_pair_at(&second, step.pair_index);
    /* One sample of the three requested is still a valid pair; only valid_samples
     * (not the PARTIAL flag) tells the consumer the set is short. */
    assert(pair->state == NRF_CS_PAIR_VALID && pair->valid_samples == 1U && pair->distance_mm == 2000);
    assert((pair->scheduler_quality_flags & NRF_CS_QUALITY_HAD_RETRY) != 0U);
    assert((pair->scheduler_quality_flags & NRF_CS_QUALITY_TIMEOUT) != 0U);
    assert((pair->scheduler_quality_flags & NRF_CS_QUALITY_PARTIAL) == 0U);

    /* Zero samples after the whole budget: exhausted, not valid. */
    nrf_cs_scheduler_t third;
    start_nodes(&third, &p, 3U);
    for (int attempt = 0; attempt < 2; ++attempt) {
        assert(nrf_cs_scheduler_next_step(&third, &step) == NRF_CS_STEP_READY);
        assert(nrf_cs_scheduler_complete_step(&third, &step, &(nrf_cs_measurement_t){
            .outcome = NRF_CS_OUTCOME_TRANSPORT_ERROR}) == NRF_CS_OK);
    }
    pair = nrf_cs_scheduler_pair_at(&third, step.pair_index);
    assert(pair->state == NRF_CS_PAIR_RETRY_EXHAUSTED && pair->attempts == 2U);
    CASE_PASS("CS.SCHEDULER.retry_budget");
}

static void disconnect_and_abort_resolve_pairs_without_inventing_distances(void)
{
    nrf_cs_scheduler_t scheduler;
    const nrf_cs_policy_t p = policy(1U, 1U, 0U, false);
    nrf_cs_step_t step;
    unsigned generation_before;

    start_nodes(&scheduler, &p, 4U);
    generation_before = scheduler.membership_generation;
    assert(nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY);
    const nrf_cs_node_id_t gone = step.initiator;
    assert(nrf_cs_scheduler_disconnect(&scheduler, gone) == NRF_CS_OK);
    assert(scheduler.membership_generation != generation_before);
    /* The in-flight step died with the node. */
    assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &(nrf_cs_measurement_t){
        .outcome = NRF_CS_OUTCOME_SUCCESS, .distance_mm = 1, .confidence = 900U}) == NRF_CS_ERR_STALE);
    assert(nrf_cs_scheduler_disconnect(&scheduler, gone) == NRF_CS_OK);
    assert(nrf_cs_scheduler_disconnect(&scheduler, 99U) == NRF_CS_ERR_NOT_FOUND);
    for (size_t i = 0U; i < nrf_cs_scheduler_pair_count(&scheduler); ++i) {
        const nrf_cs_pair_result_t *pair = nrf_cs_scheduler_pair_at(&scheduler, i);
        if (pair->node_a == gone || pair->node_b == gone) {
            assert(pair->state == NRF_CS_PAIR_MISSING && pair->distance_mm == 0 && pair->valid_samples == 0U);
        }
    }
    /* Pairs that do not involve the node still run. */
    unsigned remaining = 0U;
    while (nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY) {
        assert(step.initiator != gone && step.reflector != gone);
        assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &(nrf_cs_measurement_t){
            .outcome = NRF_CS_OUTCOME_SUCCESS, .distance_mm = 3000, .confidence = 900U}) == NRF_CS_OK);
        ++remaining;
    }
    assert(remaining == 3U && nrf_cs_scheduler_is_complete(&scheduler));
    /* The next cycle starts a new membership generation after a disconnect. */
    generation_before = scheduler.membership_generation;
    {
        const nrf_cs_node_id_t same[] = {0U, 1U, 2U, 3U};
        assert(nrf_cs_scheduler_start_cycle(&scheduler, same, 4U) == NRF_CS_OK);
        assert(scheduler.membership_generation != generation_before);
        generation_before = scheduler.membership_generation;
        for (;;) {
            if (nrf_cs_scheduler_next_step(&scheduler, &step) != NRF_CS_STEP_READY) break;
            assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &(nrf_cs_measurement_t){
                .outcome = NRF_CS_OUTCOME_SUCCESS, .distance_mm = 1000, .confidence = 900U}) == NRF_CS_OK);
        }
        assert(nrf_cs_scheduler_start_cycle(&scheduler, same, 4U) == NRF_CS_OK);
        /* Unchanged membership keeps its generation. */
        assert(scheduler.membership_generation == generation_before);
    }

    /* Abort: remaining executions end as failures, collected samples stay. */
    nrf_cs_scheduler_t aborted;
    start_nodes(&aborted, &p, 3U);
    assert(nrf_cs_scheduler_next_step(&aborted, &step) == NRF_CS_STEP_READY);
    assert(nrf_cs_scheduler_complete_step(&aborted, &step, &(nrf_cs_measurement_t){
        .outcome = NRF_CS_OUTCOME_SUCCESS, .distance_mm = 1500, .confidence = 900U}) == NRF_CS_OK);
    assert(nrf_cs_scheduler_next_step(&aborted, &step) == NRF_CS_STEP_READY);
    for (unsigned reason = 0U; reason < 8U; ++reason) {
        nrf_cs_scheduler_t copy = aborted;
        const nrf_cs_result_t result = nrf_cs_scheduler_abort_cycle(&copy, (nrf_cs_failure_reason_t)reason);
        const bool allowed = reason >= NRF_CS_FAILURE_TIMEOUT && reason <= NRF_CS_FAILURE_DISCONNECTED;
        assert(result == (allowed ? NRF_CS_OK : NRF_CS_ERR_ARGUMENT));
        if (allowed) {
            assert(nrf_cs_scheduler_is_complete(&copy));
            assert(nrf_cs_scheduler_complete_step(&copy, &step, &(nrf_cs_measurement_t){
                .outcome = NRF_CS_OUTCOME_SUCCESS, .distance_mm = 1, .confidence = 900U}) == NRF_CS_ERR_STALE);
        }
    }
    nrf_cs_scheduler_t idle;
    assert(nrf_cs_scheduler_init(&idle, &p) == NRF_CS_OK);
    assert(nrf_cs_scheduler_abort_cycle(&idle, NRF_CS_FAILURE_TIMEOUT) == NRF_CS_ERR_ARGUMENT);
    assert(nrf_cs_scheduler_disconnect(&idle, 0U) == NRF_CS_ERR_ARGUMENT);
    CASE_PASS("CS.SCHEDULER.disconnect_abort");
}

static int32_t oracle_median(int32_t *values, size_t count)
{
    for (size_t i = 1U; i < count; ++i) {
        const int32_t v = values[i];
        size_t j = i;
        while (j > 0U && values[j - 1U] > v) { values[j] = values[j - 1U]; --j; }
        values[j] = v;
    }
    return (count % 2U) == 1U ? values[count / 2U]
        : (int32_t)(((int64_t)values[count / 2U - 1U] + values[count / 2U]) / 2);
}

static nrf_cs_pair_result_t run_samples(const int32_t *distances, size_t count, uint16_t confidence)
{
    nrf_cs_scheduler_t scheduler;
    const nrf_cs_policy_t p = policy((uint8_t)count, 1U, 0U, false);
    nrf_cs_step_t step;

    start_nodes(&scheduler, &p, 3U);
    /* Pair 0 receives the scripted samples; the other pairs a trivial success. */
    while (nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY) {
        const bool scripted = step.pair_index == 0U;
        assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &(nrf_cs_measurement_t){
            .outcome = NRF_CS_OUTCOME_SUCCESS,
            .distance_mm = scripted ? distances[step.attempt_number - 1U] : 100,
            .confidence = scripted ? confidence : 900U}) == NRF_CS_OK);
    }
    return *nrf_cs_scheduler_pair_at(&scheduler, 0U);
}

static void outlier_statistics_match_an_independent_oracle(void)
{
    uint32_t seed = 0x12345678U;
    for (unsigned trial = 0U; trial < 400U; ++trial) {
        int32_t samples[NRF_CS_MAX_SAMPLES_PER_EXECUTION];
        int32_t sorted[NRF_CS_MAX_SAMPLES_PER_EXECUTION];
        const size_t count = 1U + (trial % NRF_CS_MAX_SAMPLES_PER_EXECUTION);
        int32_t base = 500 + (int32_t)((seed >> 8U) % 20000U);
        for (size_t i = 0U; i < count; ++i) {
            seed = seed * 1664525U + 1013904223U;
            int32_t jitter = (int32_t)((seed >> 16U) % 61U) - 30;
            /* Every third trial injects a gross outlier. */
            if (trial % 3U == 0U && i == count - 1U && count >= 4U) jitter = 4000;
            samples[i] = base + jitter;
        }
        const nrf_cs_pair_result_t pair = run_samples(samples, count, 800U);
        assert(pair.state == NRF_CS_PAIR_VALID && pair.valid_samples == count);

        /* Independent oracle: median, MAD, 3 sigma band (min 100 mm), median of kept. */
        memcpy(sorted, samples, count * sizeof(samples[0]));
        const int32_t median = oracle_median(sorted, count);
        int32_t deviations[NRF_CS_MAX_SAMPLES_PER_EXECUTION];
        for (size_t i = 0U; i < count; ++i) deviations[i] = samples[i] > median ? samples[i] - median : median - samples[i];
        const int32_t mad = oracle_median(deviations, count);
        const int64_t sigma = ((int64_t)mad * 14826 + 5000) / 10000;
        int64_t band = 3 * sigma;
        if (band < 100) band = 100;
        int32_t kept[NRF_CS_MAX_SAMPLES_PER_EXECUTION];
        size_t kept_count = 0U;
        for (size_t i = 0U; i < count; ++i) {
            const int64_t d = samples[i] > median ? samples[i] - median : median - samples[i];
            if (d <= band) kept[kept_count++] = samples[i];
        }
        assert(kept_count >= (count + 1U) / 2U);
        assert(pair.distance_mm == oracle_median(kept, kept_count));
        assert(pair.rejected_samples == count - kept_count);
        assert(pair.spread_mm == (count > 1U ? (int32_t)sigma : 0));
        assert(pair.confidence == 800U);
    }
    CASE_PASS("CS.SCHEDULER.outlier_statistics");
}

static void extreme_distances_never_crash_or_discard_every_sample(void)
{
    /* Two clusters 2147 km apart: nonsense physically, but it is a legal wire value. */
    const int32_t bimodal[] = {0, 0, INT32_MAX, INT32_MAX};
    nrf_cs_pair_result_t pair = run_samples(bimodal, 4U, 900U);
    assert(pair.state == NRF_CS_PAIR_VALID);
    assert(pair.valid_samples == 4U);
    assert(pair.rejected_samples <= 2U);
    assert(pair.distance_mm >= 0);

    const int32_t huge_spread[] = {0, 1000000000, 2000000000, INT32_MAX};
    pair = run_samples(huge_spread, 4U, 900U);
    assert(pair.state == NRF_CS_PAIR_VALID && pair.valid_samples == 4U);
    assert(pair.rejected_samples <= 2U);

    const int32_t max_only[] = {INT32_MAX, INT32_MAX, INT32_MAX};
    pair = run_samples(max_only, 3U, 900U);
    assert(pair.state == NRF_CS_PAIR_VALID && pair.distance_mm == INT32_MAX && pair.rejected_samples == 0U);

    const int32_t zero_only[] = {0, 0, 0, 0, 0, 0, 0, 0};
    pair = run_samples(zero_only, 8U, 900U);
    assert(pair.distance_mm == 0 && pair.spread_mm == 0 && pair.rejected_samples == 0U);
    CASE_PASS("CS.SCHEDULER.extreme_distances");
}

static void full_rotation_combines_both_directions(void)
{
    nrf_cs_scheduler_t scheduler;
    const nrf_cs_policy_t p = policy(1U, 1U, 0U, true);
    nrf_cs_step_t step;

    start_nodes(&scheduler, &p, 2U);
    /* 0 -> 1 reads 1000 mm, 1 -> 0 reads 1300 mm. */
    for (int direction = 0; direction < 2; ++direction) {
        assert(nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY);
        assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &(nrf_cs_measurement_t){
            .outcome = NRF_CS_OUTCOME_SUCCESS, .distance_mm = direction == 0 ? 1000 : 1300,
            .confidence = direction == 0 ? 800U : 600U}) == NRF_CS_OK);
    }
    const nrf_cs_pair_result_t *pair = nrf_cs_scheduler_pair_at(&scheduler, 0U);
    assert(pair->state == NRF_CS_PAIR_VALID && !pair->single_direction);
    assert(pair->distance_mm == 1150 && pair->confidence == 700U);
    assert(pair->direction_disagreement_mm == 300);
    assert(pair->spread_mm == 300);

    /* One direction failing leaves a single-direction, partial result. */
    start_nodes(&scheduler, &p, 2U);
    assert(nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY);
    assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &(nrf_cs_measurement_t){
        .outcome = NRF_CS_OUTCOME_SUCCESS, .distance_mm = 1000, .confidence = 800U}) == NRF_CS_OK);
    assert(nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY);
    assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &(nrf_cs_measurement_t){
        .outcome = NRF_CS_OUTCOME_TIMEOUT}) == NRF_CS_OK);
    pair = nrf_cs_scheduler_pair_at(&scheduler, 0U);
    assert(pair->state == NRF_CS_PAIR_VALID && pair->single_direction);
    assert(pair->distance_mm == 1000);
    assert((pair->scheduler_quality_flags & NRF_CS_QUALITY_PARTIAL) != 0U);
    CASE_PASS("CS.SCHEDULER.full_rotation");
}

int main(void)
{
    /* Progress lines must survive a crash in a later case. */
    setvbuf(stdout, NULL, _IONBF, 0);
    policy_domain_is_enforced();
    membership_and_topology_for_zero_to_five_nodes();
    every_pair_runs_exactly_once_with_unique_nonzero_attempt_ids();
    every_outcome_and_confidence_has_one_defined_effect();
    stale_steps_are_rejected_for_every_identity_field();
    only_failed_attempts_consume_the_retry_budget();
    disconnect_and_abort_resolve_pairs_without_inventing_distances();
    outlier_statistics_match_an_independent_oracle();
    extreme_distances_never_crash_or_discard_every_sample();
    full_rotation_combines_both_directions();
    return 0;
}
