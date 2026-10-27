#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sensor_bus_probe_diag_policy.h"

_Static_assert(sizeof(sensor_bus_probe_diag_group_t) <= 48U,
               "I2C probe group record must stay compact");
_Static_assert(sizeof(sensor_bus_probe_diag_policy_t) <=
                   SENSOR_BUS_PROBE_DIAG_STATIC_RAM_BUDGET_BYTES,
               "I2C probe policy exceeds static RAM budget");
_Static_assert(sizeof(sensor_bus_probe_diag_storage_t) <=
                   SENSOR_BUS_PROBE_DIAG_STATIC_RAM_BUDGET_BYTES,
               "I2C probe static storage exceeds 6144 bytes");

static sensor_bus_probe_diag_transaction_t make_transaction(
    sensor_bus_probe_phase_t phase,
    uint8_t address,
    int8_t channel,
    sensor_bus_probe_result_t result,
    int32_t err,
    uint64_t elapsed_us)
{
    const sensor_bus_probe_diag_transaction_t transaction = {
        .phase = phase,
        .address = address,
        .channel = channel,
        .result = result,
        .err = err,
        .elapsed_us = elapsed_us,
    };
    return transaction;
}

static sensor_bus_probe_diag_group_t *find_group(
    sensor_bus_probe_diag_policy_t *state,
    sensor_bus_probe_phase_t phase,
    uint8_t address,
    int8_t channel)
{
    for (size_t i = 0U; i < SENSOR_BUS_PROBE_DIAG_GROUP_LIMIT; ++i) {
        sensor_bus_probe_diag_group_t *group = &state->groups[i];
        if ((group->flags & 0x01U) != 0U &&
            sensor_bus_probe_diag_group_phase(group) == phase &&
            sensor_bus_probe_diag_group_address(group) == address &&
            sensor_bus_probe_diag_group_channel(group) == channel) {
            return group;
        }
    }
    return NULL;
}

static void test_storage_budget_is_reported(void)
{
    const size_t member_bytes = sizeof(((sensor_bus_probe_diag_storage_t *)0)->policy) +
        sizeof(((sensor_bus_probe_diag_storage_t *)0)->transaction) +
        sizeof(((sensor_bus_probe_diag_storage_t *)0)->summary) +
        sizeof(((sensor_bus_probe_diag_storage_t *)0)->tx_hex) +
        sizeof(((sensor_bus_probe_diag_storage_t *)0)->rx_hex) +
        sizeof(((sensor_bus_probe_diag_storage_t *)0)->expected_hex) +
        sizeof(((sensor_bus_probe_diag_storage_t *)0)->sda_before) +
        sizeof(((sensor_bus_probe_diag_storage_t *)0)->scl_before) +
        sizeof(((sensor_bus_probe_diag_storage_t *)0)->sda_after) +
        sizeof(((sensor_bus_probe_diag_storage_t *)0)->scl_after) +
        sizeof(((sensor_bus_probe_diag_storage_t *)0)->readback_mismatch) +
        sizeof(((sensor_bus_probe_diag_storage_t *)0)->changed) +
        sizeof(((sensor_bus_probe_diag_storage_t *)0)->should_print) +
        sizeof(((sensor_bus_probe_diag_storage_t *)0)->started_us) +
        sizeof(((sensor_bus_probe_diag_storage_t *)0)->finished_us) +
        sizeof(((sensor_bus_probe_diag_storage_t *)0)->completed_us);

    assert(sizeof(sensor_bus_probe_diag_storage_t) <=
        SENSOR_BUS_PROBE_DIAG_STATIC_RAM_BUDGET_BYTES);
    printf("I2C_PROBE_DIAG_STATIC_BYTES total=%zu policy=%zu group=%zu transaction=%zu summary=%zu tx_hex=%zu rx_hex=%zu expected_hex=%zu flags=%zu timestamps=%zu padding=%zu\n",
        sizeof(sensor_bus_probe_diag_storage_t),
        sizeof(sensor_bus_probe_diag_policy_t),
        sizeof(sensor_bus_probe_diag_group_t),
        sizeof(sensor_bus_probe_diag_transaction_t),
        sizeof(sensor_bus_probe_diag_group_t),
        sizeof(((sensor_bus_probe_diag_storage_t *)0)->tx_hex),
        sizeof(((sensor_bus_probe_diag_storage_t *)0)->rx_hex),
        sizeof(((sensor_bus_probe_diag_storage_t *)0)->expected_hex),
        sizeof(bool) * 7U,
        sizeof(((sensor_bus_probe_diag_storage_t *)0)->started_us) +
            sizeof(((sensor_bus_probe_diag_storage_t *)0)->finished_us) +
            sizeof(((sensor_bus_probe_diag_storage_t *)0)->completed_us),
        sizeof(sensor_bus_probe_diag_storage_t) - member_bytes);
}

static void test_full_pass_and_result_change_detection(void)
{
    sensor_bus_probe_diag_policy_t state;
    sensor_bus_probe_diag_transaction_t transaction = make_transaction(
        SENSOR_BUS_PROBE_PHASE_PROBE, 0x31U, -1,
        SENSOR_BUS_PROBE_RESULT_NACK, -5, 10U);
    bool changed = false;
    bool should_print = false;

    sensor_bus_probe_diag_policy_reset(&state);
    sensor_bus_probe_diag_policy_begin_pass(&state, 1U, 3U);
    assert(sensor_bus_probe_diag_policy_record(
        &state, &transaction, 100U, &changed, &should_print));
    assert(changed && should_print);
    assert(sensor_bus_probe_diag_policy_record(
        &state, &transaction, 101U, &changed, &should_print));
    assert(!changed && should_print);
    assert(sensor_bus_probe_diag_policy_end_pass(&state) == 0U);
    assert(sensor_bus_probe_diag_policy_record(
        &state, &transaction, 102U, &changed, &should_print));
    assert(!changed && !should_print);

    sensor_bus_probe_diag_policy_begin_pass(&state, 4U, 3U);
    assert(sensor_bus_probe_diag_policy_record(
        &state, &transaction, 200U, &changed, &should_print));
    assert(!changed && !should_print);

    transaction.err = -7;
    assert(sensor_bus_probe_diag_policy_record(
        &state, &transaction, 300U, &changed, &should_print));
    assert(changed && should_print);

    transaction.result = SENSOR_BUS_PROBE_RESULT_TIMEOUT;
    assert(sensor_bus_probe_diag_policy_record(
        &state, &transaction, 400U, &changed, &should_print));
    assert(changed && should_print);
    assert(sensor_bus_probe_diag_policy_record(
        &state, &transaction, 500U, &changed, &should_print));
    assert(!changed && !should_print);
    assert(sensor_bus_probe_diag_policy_end_pass(&state) == 0U);
}

static void test_group_statistics_and_sixty_second_summary(void)
{
    sensor_bus_probe_diag_policy_t state;
    sensor_bus_probe_diag_transaction_t transaction = make_transaction(
        SENSOR_BUS_PROBE_PHASE_READ, 0x32U, 7,
        SENSOR_BUS_PROBE_RESULT_OK, 0, 5U);
    sensor_bus_probe_diag_group_t summary;
    bool changed = false;
    bool should_print = false;
    const uint64_t start_us = (UINT64_C(1) << 32U) + UINT64_C(987654);

    sensor_bus_probe_diag_policy_reset(&state);
    assert(sensor_bus_probe_diag_policy_record(
        &state, &transaction, start_us, &changed, &should_print));
    transaction.elapsed_us = 15U;
    assert(sensor_bus_probe_diag_policy_record(
        &state, &transaction, start_us + 100U, &changed, &should_print));
    transaction.result = SENSOR_BUS_PROBE_RESULT_NACK;
    transaction.err = -5;
    transaction.elapsed_us = 10U;
    assert(sensor_bus_probe_diag_policy_record(
        &state, &transaction, start_us + 200U, &changed, &should_print));
    transaction.result = SENSOR_BUS_PROBE_RESULT_OTHER;
    transaction.err = -8;
    transaction.elapsed_us = 20U;
    assert(sensor_bus_probe_diag_policy_record(
        &state, &transaction, start_us + 300U, &changed, &should_print));

    assert(!sensor_bus_probe_diag_policy_take_summary(
        &state, 0U, start_us + UINT64_C(59999999), &summary));
    assert(sensor_bus_probe_diag_policy_take_summary(
        &state, 0U, start_us + UINT64_C(60000000), &summary));
    assert(summary.counts[SENSOR_BUS_PROBE_RESULT_OK] == 2U);
    assert(summary.counts[SENSOR_BUS_PROBE_RESULT_NACK] == 1U);
    assert(summary.counts[SENSOR_BUS_PROBE_RESULT_OTHER] == 1U);
    assert(summary.min_us == 5U);
    assert(summary.max_us == 20U);
    assert(summary.avg_us == 12U);
    assert(summary.sample_count == 4U);
    assert(summary.last_err == -8);
    assert(!sensor_bus_probe_diag_policy_take_summary(
        &state, 0U, start_us + UINT64_C(60000000), &summary));

    transaction.result = SENSOR_BUS_PROBE_RESULT_TIMEOUT;
    transaction.err = -7;
    transaction.elapsed_us = 40U;
    assert(sensor_bus_probe_diag_policy_record(
        &state, &transaction, start_us + UINT64_C(60000200),
        &changed, &should_print));
    assert(!sensor_bus_probe_diag_policy_take_summary(
        &state, 0U, start_us + UINT64_C(119999999), &summary));
    assert(sensor_bus_probe_diag_policy_take_summary(
        &state, 0U, start_us + UINT64_C(120000200), &summary));
    assert(summary.counts[SENSOR_BUS_PROBE_RESULT_TIMEOUT] == 1U);
    assert(summary.min_us == 40U && summary.max_us == 40U);
    assert(summary.avg_us == 40U && summary.last_err == -7);
}

static void test_summary_clock_wraps_after_uint32_seconds(void)
{
    sensor_bus_probe_diag_policy_t state;
    sensor_bus_probe_diag_transaction_t transaction = make_transaction(
        SENSOR_BUS_PROBE_PHASE_READ, 0x33U, 1,
        SENSOR_BUS_PROBE_RESULT_OK, 0, 20U);
    sensor_bus_probe_diag_group_t summary;
    bool changed = false;
    bool should_print = false;
    const uint64_t start_us =
        (uint64_t)UINT32_MAX * UINT64_C(1000000) - UINT64_C(10000000) + 123U;

    sensor_bus_probe_diag_policy_reset(&state);
    assert(sensor_bus_probe_diag_policy_record(
        &state, &transaction, start_us, &changed, &should_print));
    assert(!sensor_bus_probe_diag_policy_take_summary(
        &state, 0U, start_us + UINT64_C(59999999), &summary));
    assert(sensor_bus_probe_diag_policy_take_summary(
        &state, 0U, start_us + UINT64_C(60000000), &summary));
    assert(summary.sample_count == 1U);
    assert(summary.avg_us == 20U);
}

static void test_mismatch_bucket_and_idle_change_detection(void)
{
    sensor_bus_probe_diag_policy_t state;
    sensor_bus_probe_diag_transaction_t transaction = make_transaction(
        SENSOR_BUS_PROBE_PHASE_TCA_VERIFY, 0x70U, -1,
        SENSOR_BUS_PROBE_RESULT_OK, 0, 12U);
    sensor_bus_probe_diag_group_t *group;
    bool changed = false;
    bool should_print = false;

    sensor_bus_probe_diag_policy_reset(&state);
    sensor_bus_probe_diag_policy_begin_pass(&state, 1U, 3U);
    assert(sensor_bus_probe_diag_policy_record(
        &state, &transaction, 100U, &changed, &should_print));
    assert(changed && should_print);
    transaction.readback_mismatch = true;
    transaction.result = SENSOR_BUS_PROBE_RESULT_READBACK_MISMATCH;
    assert(sensor_bus_probe_diag_policy_record(
        &state, &transaction, 101U, &changed, &should_print));
    assert(changed && should_print);
    assert(sensor_bus_probe_diag_policy_record(
        &state, &transaction, 102U, &changed, &should_print));
    assert(!changed && should_print);
    group = find_group(
        &state, SENSOR_BUS_PROBE_PHASE_TCA_VERIFY, 0x70U, -1);
    assert(group != NULL);
    assert(sensor_bus_probe_diag_group_phase(group) ==
        SENSOR_BUS_PROBE_PHASE_TCA_VERIFY);
    assert(sensor_bus_probe_diag_group_address(group) == 0x70U);
    assert(sensor_bus_probe_diag_group_channel(group) == -1);
    assert(group->counts[SENSOR_BUS_PROBE_RESULT_OK] == 1U);
    assert(group->counts[SENSOR_BUS_PROBE_RESULT_READBACK_MISMATCH] == 2U);

    assert(sensor_bus_probe_diag_policy_idle_changed(&state, 1, 1));
    assert(sensor_bus_probe_diag_policy_idle_changed(&state, 1, 1));
    assert(sensor_bus_probe_diag_policy_end_pass(&state) == 0U);
    sensor_bus_probe_diag_policy_begin_pass(&state, 4U, 3U);
    assert(!sensor_bus_probe_diag_policy_idle_changed(&state, 1, 1));
    assert(sensor_bus_probe_diag_policy_idle_changed(&state, 0, 1));
    assert(sensor_bus_probe_diag_policy_end_pass(&state) == 0U);
}

static void test_counter_and_elapsed_saturation(void)
{
    sensor_bus_probe_diag_policy_t state;
    sensor_bus_probe_diag_transaction_t transaction = make_transaction(
        SENSOR_BUS_PROBE_PHASE_WRITE, 0x31U, -1,
        SENSOR_BUS_PROBE_RESULT_OK, 0, 2U);
    sensor_bus_probe_diag_group_t *group;
    bool changed = false;
    bool should_print = false;

    sensor_bus_probe_diag_policy_reset(&state);
    assert(sensor_bus_probe_diag_policy_record(
        &state, &transaction, 100U, &changed, &should_print));
    group = find_group(&state, SENSOR_BUS_PROBE_PHASE_WRITE, 0x31U, -1);
    assert(group != NULL);
    for (size_t i = 0U; i < (size_t)SENSOR_BUS_PROBE_RESULT_COUNT; ++i) {
        group->counts[i] = UINT16_MAX;
    }
    transaction.elapsed_us = UINT64_MAX;
    assert(sensor_bus_probe_diag_policy_record(
        &state, &transaction, 101U, &changed, &should_print));
    for (size_t i = 0U; i < (size_t)SENSOR_BUS_PROBE_RESULT_COUNT; ++i) {
        assert(group->counts[i] == UINT16_MAX);
    }
    assert(group->min_us == 2U);
    assert(group->max_us == UINT32_MAX);
    assert(group->avg_us == UINT32_C(2147483648));

    group->sample_count = UINT32_MAX;
    transaction.elapsed_us = UINT64_MAX;
    assert(sensor_bus_probe_diag_policy_record(
        &state, &transaction, 102U, &changed, &should_print));
    assert(group->sample_count == 1U);
    assert(group->avg_us == UINT32_MAX);
    assert(group->min_us == 2U && group->max_us == UINT32_MAX);
    assert(sensor_bus_probe_diag_saturating_add_u32(UINT32_MAX, 1U) == UINT32_MAX);
}

static void test_group_cap_overflow_is_counted_and_printed(void)
{
    sensor_bus_probe_diag_policy_t state;
    sensor_bus_probe_diag_transaction_t transaction;
    bool changed = false;
    bool should_print = false;

    sensor_bus_probe_diag_policy_reset(&state);
    sensor_bus_probe_diag_policy_begin_pass(&state, 4U, 3U);
    for (unsigned i = 0U; i < SENSOR_BUS_PROBE_DIAG_GROUP_LIMIT; ++i) {
        transaction = make_transaction(
            SENSOR_BUS_PROBE_PHASE_PROBE, (uint8_t)i, -1,
            SENSOR_BUS_PROBE_RESULT_OK, 0, 12U);
        assert(sensor_bus_probe_diag_policy_record(
            &state, &transaction, (uint64_t)i, &changed, &should_print));
        assert(changed && should_print);
    }
    assert(find_group(&state, SENSOR_BUS_PROBE_PHASE_PROBE, 0U, -1) != NULL);

    transaction = make_transaction(
        SENSOR_BUS_PROBE_PHASE_WRITE, 0x40U, -1,
        SENSOR_BUS_PROBE_RESULT_NACK, -5, 14U);
    assert(sensor_bus_probe_diag_policy_record(
        &state, &transaction, 100U, &changed, &should_print));
    assert(changed && should_print);
    assert(state.groups_dropped == 1U);
    assert(find_group(&state, SENSOR_BUS_PROBE_PHASE_PROBE, 0U, -1) != NULL);

    assert(sensor_bus_probe_diag_policy_record(
        &state, &transaction, 101U, &changed, &should_print));
    assert(changed && should_print);
    assert(state.groups_dropped == 2U);
    state.groups_dropped = UINT32_MAX - 1U;
    assert(sensor_bus_probe_diag_policy_record(
        &state, &transaction, 102U, &changed, &should_print));
    assert(sensor_bus_probe_diag_policy_record(
        &state, &transaction, 103U, &changed, &should_print));
    assert(state.groups_dropped == UINT32_MAX);

    /* Healthy untracked traffic is counted but not printed after the full passes. */
    transaction = make_transaction(
        SENSOR_BUS_PROBE_PHASE_WRITE, 0x41U, -1,
        SENSOR_BUS_PROBE_RESULT_OK, 0, 14U);
    assert(sensor_bus_probe_diag_policy_record(
        &state, &transaction, 104U, &changed, &should_print));
    assert(!should_print);
    assert(state.groups_dropped == UINT32_MAX);
    state.groups_dropped = 5U;
    assert(sensor_bus_probe_diag_policy_record(
        &state, &transaction, 105U, &changed, &should_print));
    assert(!should_print && state.groups_dropped == 6U);
    /* During a full pass it still prints. */
    sensor_bus_probe_diag_policy_begin_pass(&state, 1U, 3U);
    assert(sensor_bus_probe_diag_policy_record(
        &state, &transaction, 106U, &changed, &should_print));
    assert(should_print && state.groups_dropped == 7U);
    sensor_bus_probe_diag_policy_begin_pass(&state, 4U, 3U);
    state.groups_dropped = UINT32_MAX;
    assert(sensor_bus_probe_diag_policy_end_pass(&state) == UINT32_MAX);
    assert(state.groups_dropped == 0U);
    assert(sensor_bus_probe_diag_policy_end_pass(&state) == 0U);
}

static void test_overflow_routes_report_changes_and_recovery(void)
{
    sensor_bus_probe_diag_policy_t state;
    sensor_bus_probe_diag_transaction_t transaction;
    bool changed;
    bool should_print;

    sensor_bus_probe_diag_policy_reset(&state);
    sensor_bus_probe_diag_policy_begin_pass(&state, 4U, 3U);
    for (unsigned i = 0U; i < SENSOR_BUS_PROBE_DIAG_GROUP_LIMIT; ++i) {
        transaction = make_transaction(SENSOR_BUS_PROBE_PHASE_PROBE, (uint8_t)i, -1,
                                       SENSOR_BUS_PROBE_RESULT_OK, 0, 1U);
        assert(sensor_bus_probe_diag_policy_record(&state, &transaction, i, &changed, &should_print));
    }
    transaction = make_transaction(SENSOR_BUS_PROBE_PHASE_WRITE, 0x31U, -1,
                                   SENSOR_BUS_PROBE_RESULT_OTHER, 0x103, 1U);
    assert(sensor_bus_probe_diag_policy_record(&state, &transaction, 100U, &changed, &should_print));
    assert(changed && should_print);
    for (unsigned i = 0U; i < 20U; ++i) {
        assert(sensor_bus_probe_diag_policy_record(&state, &transaction, 101U + i, &changed, &should_print));
        assert(!changed && !should_print);
    }
    transaction.err = 0x104;
    assert(sensor_bus_probe_diag_policy_record(&state, &transaction, 121U, &changed, &should_print));
    assert(changed && should_print);
    transaction.address = 0x32U;
    transaction.channel = 3;
    assert(sensor_bus_probe_diag_policy_record(&state, &transaction, 122U, &changed, &should_print));
    assert(changed && should_print);
    transaction.address = 0x31U;
    transaction.channel = -1;
    assert(sensor_bus_probe_diag_policy_record(&state, &transaction, 123U, &changed, &should_print));
    assert(!changed && !should_print);
    transaction.result = SENSOR_BUS_PROBE_RESULT_OK;
    transaction.err = 0;
    assert(sensor_bus_probe_diag_policy_record(&state, &transaction, 124U, &changed, &should_print));
    assert(changed && should_print);
    assert(sensor_bus_probe_diag_policy_record(&state, &transaction, 125U, &changed, &should_print));
    assert(!changed && !should_print);
    transaction.address = 0x32U;
    transaction.channel = 3;
    assert(sensor_bus_probe_diag_policy_record(&state, &transaction, 126U, &changed, &should_print));
    assert(changed && should_print);
}

static void test_every_probe_route_has_independent_overflow_state(void)
{
    sensor_bus_probe_diag_policy_t state;
    sensor_bus_probe_diag_transaction_t transaction;
    bool changed;
    bool should_print;
    sensor_bus_probe_diag_policy_reset(&state);
    sensor_bus_probe_diag_policy_begin_pass(&state, 4U, 3U);
    for (unsigned i = 0U; i < SENSOR_BUS_PROBE_DIAG_GROUP_LIMIT; ++i) {
        transaction = make_transaction(SENSOR_BUS_PROBE_PHASE_PROBE, (uint8_t)i, -1,
                                       SENSOR_BUS_PROBE_RESULT_OK, 0, 1U);
        assert(sensor_bus_probe_diag_policy_record(&state, &transaction, i, &changed, &should_print));
    }
    for (unsigned round = 0U; round < 3U; ++round) {
        for (unsigned phase = 0U; phase < SENSOR_BUS_PROBE_PHASE_COUNT; ++phase) {
            for (unsigned address = 0U; address < 9U; ++address) {
                for (int channel = -1; channel < 8; ++channel) {
                    const uint8_t wire_address = (uint8_t)(address < 3U
                        ? 0x30U + address : 0x70U + address - 3U);
                    transaction = make_transaction((sensor_bus_probe_phase_t)phase,
                        wire_address, (int8_t)channel,
                        round == 2U ? SENSOR_BUS_PROBE_RESULT_OK : SENSOR_BUS_PROBE_RESULT_OTHER,
                        round == 2U ? 0 : (int32_t)(100U + phase * 9U + address), 1U);
                    assert(sensor_bus_probe_diag_policy_record(&state, &transaction, 200U,
                                                                &changed, &should_print));
                    assert(changed == (round != 1U));
                    assert(should_print == (round != 1U));
                }
            }
        }
    }
    assert(state.groups_dropped == 3U * SENSOR_BUS_PROBE_DIAG_OVERFLOW_ROUTES);
}

static void test_hex_formatting_is_bounded_to_four_bytes(void)
{
    const uint8_t bytes[] = { 0x00U, 0x7FU, 0xA5U, 0xFFU, 0x42U };
    char output[9];
    char too_small[8];

    assert(sensor_bus_probe_diag_format_hex(output, sizeof(output), bytes, 5U));
    assert(strcmp(output, "007FA5FF") == 0);
    assert(sensor_bus_probe_diag_format_hex(output, sizeof(output), NULL, 0U));
    assert(strcmp(output, "-") == 0);
    assert(!sensor_bus_probe_diag_format_hex(
        too_small, sizeof(too_small), bytes, 4U));
    assert(too_small[0] == '\0');
    assert(strcmp(sensor_bus_probe_diag_phase_name(
        SENSOR_BUS_PROBE_PHASE_TCA_VERIFY), "tca_verify") == 0);
}

int main(void)
{
    test_storage_budget_is_reported();
    test_full_pass_and_result_change_detection();
    test_group_statistics_and_sixty_second_summary();
    test_summary_clock_wraps_after_uint32_seconds();
    test_mismatch_bucket_and_idle_change_detection();
    test_counter_and_elapsed_saturation();
    test_group_cap_overflow_is_counted_and_printed();
    test_overflow_routes_report_changes_and_recovery();
    test_every_probe_route_has_independent_overflow_state();
    test_hex_formatting_is_bounded_to_four_bytes();
    return 0;
}
