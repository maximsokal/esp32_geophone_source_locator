#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "nrf_stage_signal.h"

typedef struct {
    uint32_t offset_ms;
    bool overlay_active;
    bool green;
    bool yellow;
    bool red;
} expected_sample_t;

static void init_ok(nrf_stage_signal_t *state, uint32_t timeout_ms)
{
    assert(nrf_stage_signal_init(state, timeout_ms) == NRF_STAGE_SIGNAL_OK);
}

static nrf_stage_signal_output_t update_ok(
    nrf_stage_signal_t *state,
    uint32_t now_ms)
{
    nrf_stage_signal_output_t output = {0};
    assert(nrf_stage_signal_update(state, now_ms, &output) == NRF_STAGE_SIGNAL_OK);
    return output;
}

static void assert_output(
    nrf_stage_signal_output_t output,
    bool overlay_active,
    bool green,
    bool yellow,
    bool red)
{
    assert(output.overlay_active == overlay_active);
    assert(output.green == green);
    assert(output.yellow == yellow);
    assert(output.red == red);
    assert((output.green ? 1 : 0) + (output.yellow ? 1 : 0) +
           (output.red ? 1 : 0) <= 1);
}

static void run_timeline(
    nrf_stage_signal_event_t event,
    uint32_t start_ms,
    const expected_sample_t *samples,
    size_t sample_count)
{
    nrf_stage_signal_t state;
    nrf_stage_signal_event_t started;

    init_ok(&state, 5000U);
    assert(nrf_stage_signal_post_event(&state, event, start_ms) ==
           NRF_STAGE_SIGNAL_OK);
    assert(nrf_stage_signal_started_event(&state, &started) ==
           NRF_STAGE_SIGNAL_OK);
    assert(started == event);
    assert(nrf_stage_signal_started_event(&state, &started) ==
           NRF_STAGE_SIGNAL_ERR_NO_EVENT);

    for (size_t index = 0U; index < sample_count; ++index) {
        nrf_stage_signal_output_t output = update_ok(
            &state, start_ms + samples[index].offset_ms);
        assert_output(
            output,
            samples[index].overlay_active,
            samples[index].green,
            samples[index].yellow,
            samples[index].red);
    }
}

static void test_exact_event_timelines(void)
{
    static const expected_sample_t i2c_samples[] = {
        {0U, true, false, false, false},
        {299U, true, false, false, false},
        {300U, true, true, false, false},
        {449U, true, true, false, false},
        {450U, true, false, false, false},
        {599U, true, false, false, false},
        {600U, true, false, false, false},
        {899U, true, false, false, false},
        {900U, false, false, false, false},
    };
    static const expected_sample_t role_samples[] = {
        {0U, true, false, false, false},
        {299U, true, false, false, false},
        {300U, true, true, false, false},
        {449U, true, true, false, false},
        {450U, true, false, false, false},
        {599U, true, false, false, false},
        {600U, true, true, false, false},
        {749U, true, true, false, false},
        {750U, true, false, false, false},
        {899U, true, false, false, false},
        {900U, true, false, false, false},
        {1199U, true, false, false, false},
        {1200U, false, false, false, false},
    };
    static const expected_sample_t ready_samples[] = {
        {0U, true, false, false, false},
        {299U, true, false, false, false},
        {300U, true, true, false, false},
        {449U, true, true, false, false},
        {450U, true, false, false, false},
        {599U, true, false, false, false},
        {600U, true, true, false, false},
        {749U, true, true, false, false},
        {750U, true, false, false, false},
        {899U, true, false, false, false},
        {900U, true, true, false, false},
        {1049U, true, true, false, false},
        {1050U, true, false, false, false},
        {1199U, true, false, false, false},
        {1200U, true, false, false, false},
        {1499U, true, false, false, false},
        {1500U, false, false, false, false},
    };
    static const expected_sample_t failed_samples[] = {
        {0U, true, false, false, false},
        {299U, true, false, false, false},
        {300U, true, false, false, true},
        {449U, true, false, false, true},
        {450U, true, false, false, false},
        {599U, true, false, false, false},
        {600U, true, false, false, true},
        {749U, true, false, false, true},
        {750U, true, false, false, false},
        {899U, true, false, false, false},
        {900U, true, false, false, true},
        {1049U, true, false, false, true},
        {1050U, true, false, false, false},
        {1199U, true, false, false, false},
        {1200U, true, false, false, false},
        {1499U, true, false, false, false},
        {1500U, false, false, false, false},
    };

    run_timeline(
        NRF_STAGE_SIGNAL_EVENT_I2C_ALIVE,
        1000U,
        i2c_samples,
        sizeof(i2c_samples) / sizeof(i2c_samples[0]));
    run_timeline(
        NRF_STAGE_SIGNAL_EVENT_ROLE_REMOTE,
        2000U,
        role_samples,
        sizeof(role_samples) / sizeof(role_samples[0]));
    run_timeline(
        NRF_STAGE_SIGNAL_EVENT_SENSORS_READY,
        3000U,
        ready_samples,
        sizeof(ready_samples) / sizeof(ready_samples[0]));
    run_timeline(
        NRF_STAGE_SIGNAL_EVENT_SENSORS_FAILED,
        4000U,
        failed_samples,
        sizeof(failed_samples) / sizeof(failed_samples[0]));
}

static void assert_started(
    nrf_stage_signal_t *state,
    nrf_stage_signal_event_t expected)
{
    nrf_stage_signal_event_t started;
    assert(nrf_stage_signal_started_event(state, &started) ==
           NRF_STAGE_SIGNAL_OK);
    assert(started == expected);
}

static void test_fifo_overflow_and_identical_events(void)
{
    nrf_stage_signal_t state;
    nrf_stage_signal_output_t output;

    init_ok(&state, 10000U);
    assert(nrf_stage_signal_post_event(
               &state, NRF_STAGE_SIGNAL_EVENT_I2C_ALIVE, 0U) ==
           NRF_STAGE_SIGNAL_OK);
    assert_started(&state, NRF_STAGE_SIGNAL_EVENT_I2C_ALIVE);
    assert(nrf_stage_signal_post_event(
               &state, NRF_STAGE_SIGNAL_EVENT_ROLE_REMOTE, 1U) ==
           NRF_STAGE_SIGNAL_OK);
    assert(nrf_stage_signal_post_event(
               &state, NRF_STAGE_SIGNAL_EVENT_SENSORS_READY, 2U) ==
           NRF_STAGE_SIGNAL_OK);
    assert(nrf_stage_signal_post_event(
               &state, NRF_STAGE_SIGNAL_EVENT_SENSORS_FAILED, 3U) ==
           NRF_STAGE_SIGNAL_OK);
    assert(nrf_stage_signal_post_event(
               &state, NRF_STAGE_SIGNAL_EVENT_I2C_ALIVE, 4U) ==
           NRF_STAGE_SIGNAL_OK);
    assert(nrf_stage_signal_post_event(
               &state, NRF_STAGE_SIGNAL_EVENT_ROLE_REMOTE, 5U) ==
           NRF_STAGE_SIGNAL_ERR_QUEUE_FULL);
    assert(nrf_stage_signal_dropped_total(&state) == 1U);

    output = update_ok(&state, 899U);
    assert_output(output, true, false, false, false);
    output = update_ok(&state, 900U);
    assert_output(output, true, false, false, false);
    assert_started(&state, NRF_STAGE_SIGNAL_EVENT_ROLE_REMOTE);
    output = update_ok(&state, 1200U);
    assert_output(output, true, true, false, false);
    output = update_ok(&state, 2100U);
    assert_output(output, true, false, false, false);
    assert_started(&state, NRF_STAGE_SIGNAL_EVENT_SENSORS_READY);
    output = update_ok(&state, 2400U);
    assert_output(output, true, true, false, false);
    output = update_ok(&state, 3600U);
    assert_output(output, true, false, false, false);
    assert_started(&state, NRF_STAGE_SIGNAL_EVENT_SENSORS_FAILED);
    output = update_ok(&state, 3900U);
    assert_output(output, true, false, false, true);
    output = update_ok(&state, 5100U);
    assert_output(output, true, false, false, false);
    assert_started(&state, NRF_STAGE_SIGNAL_EVENT_I2C_ALIVE);
    assert(nrf_stage_signal_dropped_total(&state) == 1U);

    init_ok(&state, 10000U);
    assert(nrf_stage_signal_post_event(
               &state, NRF_STAGE_SIGNAL_EVENT_I2C_ALIVE, 0U) ==
           NRF_STAGE_SIGNAL_OK);
    assert(nrf_stage_signal_post_event(
               &state, NRF_STAGE_SIGNAL_EVENT_I2C_ALIVE, 1U) ==
           NRF_STAGE_SIGNAL_OK);
    assert_started(&state, NRF_STAGE_SIGNAL_EVENT_I2C_ALIVE);
    output = update_ok(&state, 900U);
    assert_output(output, true, false, false, false);
    assert_started(&state, NRF_STAGE_SIGNAL_EVENT_I2C_ALIVE);
    output = update_ok(&state, 1800U);
    assert_output(output, false, false, false, false);
    assert(nrf_stage_signal_started_event(&state, &(nrf_stage_signal_event_t){0}) ==
           NRF_STAGE_SIGNAL_ERR_NO_EVENT);
}

static void test_link_timeout_restoration_and_single_alive_event(void)
{
    nrf_stage_signal_t state;
    nrf_stage_signal_output_t output;

    init_ok(&state, 1000U);
    output = update_ok(&state, 500U);
    assert_output(output, false, false, false, false);
    assert(!nrf_stage_signal_link_lost(&state));

    assert(nrf_stage_signal_note_transactions(&state, 7U, 600U) ==
           NRF_STAGE_SIGNAL_OK);
    assert_started(&state, NRF_STAGE_SIGNAL_EVENT_I2C_ALIVE);
    output = update_ok(&state, 600U);
    assert_output(output, true, false, false, false);
    assert(nrf_stage_signal_note_transactions(&state, 7U, 1599U) ==
           NRF_STAGE_SIGNAL_OK);
    assert(!nrf_stage_signal_link_lost(&state));
    output = update_ok(&state, 1599U);
    assert_output(output, false, false, false, false);

    assert(nrf_stage_signal_note_transactions(&state, 7U, 1600U) ==
           NRF_STAGE_SIGNAL_OK);
    assert(nrf_stage_signal_link_lost(&state));
    output = update_ok(&state, 1600U);
    assert_output(output, true, false, true, false);
    output = update_ok(&state, 1749U);
    assert_output(output, true, false, true, false);
    output = update_ok(&state, 1750U);
    assert_output(output, false, false, false, false);
    output = update_ok(&state, 3600U);
    assert_output(output, true, false, true, false);

    assert(nrf_stage_signal_note_transactions(&state, 8U, 3601U) ==
           NRF_STAGE_SIGNAL_OK);
    assert(!nrf_stage_signal_link_lost(&state));
    output = update_ok(&state, 3601U);
    assert_output(output, false, false, false, false);
    assert(nrf_stage_signal_note_transactions(&state, 9U, 3602U) ==
           NRF_STAGE_SIGNAL_OK);
    assert(nrf_stage_signal_started_event(&state, &(nrf_stage_signal_event_t){0}) ==
           NRF_STAGE_SIGNAL_ERR_NO_EVENT);
}

static void test_zero_counts_do_not_start_link_until_first_transaction(void)
{
    static const uint32_t zero_observations[] = {0U, 600U, 1200U, 2001U};
    nrf_stage_signal_t state;
    nrf_stage_signal_output_t output;
    nrf_stage_signal_event_t started;

    init_ok(&state, 1000U);
    for (size_t index = 0U;
         index < sizeof(zero_observations) / sizeof(zero_observations[0]);
         ++index) {
        uint32_t now_ms = zero_observations[index];
        assert(nrf_stage_signal_note_transactions(&state, 0U, now_ms) ==
               NRF_STAGE_SIGNAL_OK);
        assert(!nrf_stage_signal_link_lost(&state));
        assert(nrf_stage_signal_started_event(&state, &started) ==
               NRF_STAGE_SIGNAL_ERR_NO_EVENT);
        output = update_ok(&state, now_ms);
        assert_output(output, false, false, false, false);
    }

    assert(!state.has_transactions);
    assert(nrf_stage_signal_note_transactions(&state, 5U, 3000U) ==
           NRF_STAGE_SIGNAL_OK);
    assert(state.has_transactions);
    assert(state.last_transaction_count == 5U);
    assert(state.last_transaction_change_ms == 3000U);
    assert(state.event_last_ms == 3000U);
    assert_started(&state, NRF_STAGE_SIGNAL_EVENT_I2C_ALIVE);
    output = update_ok(&state, 3000U);
    assert_output(output, true, false, false, false);
    output = update_ok(&state, 3299U);
    assert_output(output, true, false, false, false);
    output = update_ok(&state, 3300U);
    assert_output(output, true, true, false, false);
    output = update_ok(&state, 3900U);
    assert_output(output, false, false, false, false);

    assert(nrf_stage_signal_note_transactions(&state, 5U, 3999U) ==
           NRF_STAGE_SIGNAL_OK);
    assert(!nrf_stage_signal_link_lost(&state));
    assert(nrf_stage_signal_note_transactions(&state, 5U, 4000U) ==
           NRF_STAGE_SIGNAL_OK);
    assert(nrf_stage_signal_link_lost(&state));
    output = update_ok(&state, 4000U);
    assert_output(output, true, false, true, false);
    assert(nrf_stage_signal_started_event(&state, &started) ==
           NRF_STAGE_SIGNAL_ERR_NO_EVENT);
}

static void test_three_zero_counts_then_first_count_one(void)
{
    nrf_stage_signal_t state;
    nrf_stage_signal_output_t output;
    nrf_stage_signal_event_t started;

    init_ok(&state, 1000U);
    assert(nrf_stage_signal_note_transactions(&state, 0U, 0U) ==
           NRF_STAGE_SIGNAL_OK);
    assert(nrf_stage_signal_note_transactions(&state, 0U, 10U) ==
           NRF_STAGE_SIGNAL_OK);
    assert(nrf_stage_signal_note_transactions(&state, 0U, 20U) ==
           NRF_STAGE_SIGNAL_OK);
    assert(nrf_stage_signal_started_event(&state, &started) ==
           NRF_STAGE_SIGNAL_ERR_NO_EVENT);
    assert(!nrf_stage_signal_link_lost(&state));

    assert(nrf_stage_signal_note_transactions(&state, 1U, 100U) ==
           NRF_STAGE_SIGNAL_OK);
    assert(state.last_transaction_change_ms == 100U);
    assert(state.event_last_ms == 100U);
    assert_started(&state, NRF_STAGE_SIGNAL_EVENT_I2C_ALIVE);
    output = update_ok(&state, 100U);
    assert_output(output, true, false, false, false);
    assert(nrf_stage_signal_note_transactions(&state, 1U, 1099U) ==
           NRF_STAGE_SIGNAL_OK);
    assert(!nrf_stage_signal_link_lost(&state));
    assert(nrf_stage_signal_note_transactions(&state, 1U, 1100U) ==
           NRF_STAGE_SIGNAL_OK);
    assert(nrf_stage_signal_link_lost(&state));
}

static void test_wrap_safe_event_and_transaction_count(void)
{
    nrf_stage_signal_t state;
    nrf_stage_signal_output_t output;
    uint32_t start_ms = UINT32_MAX - 100U;

    init_ok(&state, 100U);
    assert(nrf_stage_signal_post_event(
               &state, NRF_STAGE_SIGNAL_EVENT_ROLE_REMOTE, start_ms) ==
           NRF_STAGE_SIGNAL_OK);
    output = update_ok(&state, start_ms);
    assert_output(output, true, false, false, false);
    output = update_ok(&state, 198U);
    assert_output(output, true, false, false, false);
    output = update_ok(&state, 199U);
    assert_output(output, true, true, false, false);
    output = update_ok(&state, 348U);
    assert_output(output, true, true, false, false);
    output = update_ok(&state, 349U);
    assert_output(output, true, false, false, false);

    init_ok(&state, 100U);
    assert(nrf_stage_signal_note_transactions(
               &state, UINT32_MAX, UINT32_MAX - 50U) == NRF_STAGE_SIGNAL_OK);
    assert_started(&state, NRF_STAGE_SIGNAL_EVENT_I2C_ALIVE);
    assert(nrf_stage_signal_note_transactions(&state, UINT32_MAX, 48U) ==
           NRF_STAGE_SIGNAL_OK);
    assert(!nrf_stage_signal_link_lost(&state));
    assert(nrf_stage_signal_note_transactions(&state, UINT32_MAX, 49U) ==
           NRF_STAGE_SIGNAL_OK);
    assert(nrf_stage_signal_link_lost(&state));
    assert(nrf_stage_signal_note_transactions(&state, 0U, 50U) ==
           NRF_STAGE_SIGNAL_OK);
    assert(!nrf_stage_signal_link_lost(&state));
    assert(nrf_stage_signal_started_event(&state, &(nrf_stage_signal_event_t){0}) ==
           NRF_STAGE_SIGNAL_ERR_NO_EVENT);
}

static void test_suppression_pauses_events_and_pending_queue(void)
{
    nrf_stage_signal_t state;
    nrf_stage_signal_output_t output;

    init_ok(&state, 10000U);
    assert(nrf_stage_signal_post_event(
               &state, NRF_STAGE_SIGNAL_EVENT_I2C_ALIVE, 0U) ==
           NRF_STAGE_SIGNAL_OK);
    output = update_ok(&state, 0U);
    assert_output(output, true, false, false, false);
    output = update_ok(&state, 100U);
    assert_output(output, true, false, false, false);

    assert(nrf_stage_signal_set_suppressed(&state, true) == NRF_STAGE_SIGNAL_OK);
    output = update_ok(&state, 500U);
    assert_output(output, false, false, false, false);
    assert(nrf_stage_signal_post_event(
               &state, NRF_STAGE_SIGNAL_EVENT_ROLE_REMOTE, 500U) ==
           NRF_STAGE_SIGNAL_OK);
    assert(nrf_stage_signal_set_suppressed(&state, false) == NRF_STAGE_SIGNAL_OK);
    output = update_ok(&state, 600U);
    assert_output(output, true, false, false, false);
    output = update_ok(&state, 799U);
    assert_output(output, true, false, false, false);
    output = update_ok(&state, 800U);
    assert_output(output, true, true, false, false);
    output = update_ok(&state, 1400U);
    assert_output(output, true, false, false, false);
    assert_started(&state, NRF_STAGE_SIGNAL_EVENT_ROLE_REMOTE);
}

static void test_suppression_pauses_lost_flash(void)
{
    nrf_stage_signal_t state;
    nrf_stage_signal_output_t output;

    init_ok(&state, 1000U);
    assert(nrf_stage_signal_note_transactions(&state, 1U, 0U) ==
           NRF_STAGE_SIGNAL_OK);
    output = update_ok(&state, 0U);
    assert_output(output, true, false, false, false);
    output = update_ok(&state, 900U);
    assert_output(output, false, false, false, false);
    assert(nrf_stage_signal_note_transactions(&state, 1U, 1000U) ==
           NRF_STAGE_SIGNAL_OK);
    assert(nrf_stage_signal_link_lost(&state));
    output = update_ok(&state, 1000U);
    assert_output(output, true, false, true, false);
    output = update_ok(&state, 1050U);
    assert_output(output, true, false, true, false);

    assert(nrf_stage_signal_set_suppressed(&state, true) == NRF_STAGE_SIGNAL_OK);
    output = update_ok(&state, 2000U);
    assert_output(output, false, false, false, false);
    assert(nrf_stage_signal_set_suppressed(&state, false) == NRF_STAGE_SIGNAL_OK);
    output = update_ok(&state, 3000U);
    assert_output(output, true, false, true, false);
    output = update_ok(&state, 3099U);
    assert_output(output, true, false, true, false);
    output = update_ok(&state, 3100U);
    assert_output(output, false, false, false, false);
}

static void test_stage_events_pause_lower_priority_lost_flash(void)
{
    nrf_stage_signal_t state;
    nrf_stage_signal_output_t output;

    init_ok(&state, 100U);
    assert(nrf_stage_signal_note_transactions(&state, 1U, 0U) ==
           NRF_STAGE_SIGNAL_OK);
    output = update_ok(&state, 0U);
    assert_output(output, true, false, false, false);
    assert(nrf_stage_signal_note_transactions(&state, 1U, 100U) ==
           NRF_STAGE_SIGNAL_OK);
    assert(nrf_stage_signal_link_lost(&state));
    output = update_ok(&state, 100U);
    assert_output(output, true, false, false, false);
    output = update_ok(&state, 900U);
    assert_output(output, true, false, true, false);
    output = update_ok(&state, 1049U);
    assert_output(output, true, false, true, false);
    output = update_ok(&state, 1050U);
    assert_output(output, false, false, false, false);
}

static void test_arguments_and_event_names(void)
{
    nrf_stage_signal_t state;
    nrf_stage_signal_output_t output = {0};
    nrf_stage_signal_event_t event = NRF_STAGE_SIGNAL_EVENT_I2C_ALIVE;

    assert(nrf_stage_signal_init(NULL, 1U) == NRF_STAGE_SIGNAL_ERR_ARGUMENT);
    assert(nrf_stage_signal_init(&state, 0U) == NRF_STAGE_SIGNAL_ERR_ARGUMENT);
    init_ok(&state, 1U);
    assert(nrf_stage_signal_post_event(NULL, event, 0U) ==
           NRF_STAGE_SIGNAL_ERR_ARGUMENT);
    assert(nrf_stage_signal_post_event(
               &state, (nrf_stage_signal_event_t)-1, 0U) ==
           NRF_STAGE_SIGNAL_ERR_ARGUMENT);
    assert(nrf_stage_signal_post_event(
               &state, NRF_STAGE_SIGNAL_EVENT_COUNT, 0U) ==
           NRF_STAGE_SIGNAL_ERR_ARGUMENT);
    assert(nrf_stage_signal_note_transactions(NULL, 0U, 0U) ==
           NRF_STAGE_SIGNAL_ERR_ARGUMENT);
    assert(nrf_stage_signal_set_suppressed(NULL, true) ==
           NRF_STAGE_SIGNAL_ERR_ARGUMENT);
    assert(nrf_stage_signal_update(NULL, 0U, &output) ==
           NRF_STAGE_SIGNAL_ERR_ARGUMENT);
    assert(nrf_stage_signal_update(&state, 0U, NULL) ==
           NRF_STAGE_SIGNAL_ERR_ARGUMENT);
    assert(nrf_stage_signal_started_event(NULL, &event) ==
           NRF_STAGE_SIGNAL_ERR_ARGUMENT);
    assert(nrf_stage_signal_started_event(&state, NULL) ==
           NRF_STAGE_SIGNAL_ERR_ARGUMENT);
    assert(nrf_stage_signal_started_event(&state, &event) ==
           NRF_STAGE_SIGNAL_ERR_NO_EVENT);
    assert(!nrf_stage_signal_link_lost(NULL));
    assert(nrf_stage_signal_dropped_total(NULL) == 0U);

    assert(strcmp(
               nrf_stage_signal_event_name(NRF_STAGE_SIGNAL_EVENT_I2C_ALIVE),
               "I2C_ALIVE") == 0);
    assert(strcmp(
               nrf_stage_signal_event_name(NRF_STAGE_SIGNAL_EVENT_ROLE_REMOTE),
               "ROLE_REMOTE") == 0);
    assert(strcmp(
               nrf_stage_signal_event_name(NRF_STAGE_SIGNAL_EVENT_SENSORS_READY),
               "SENSORS_READY") == 0);
    assert(strcmp(
               nrf_stage_signal_event_name(NRF_STAGE_SIGNAL_EVENT_SENSORS_FAILED),
               "SENSORS_FAILED") == 0);
    assert(strcmp(
               nrf_stage_signal_event_name((nrf_stage_signal_event_t)-1),
               "UNKNOWN") == 0);
}

int main(void)
{
    test_exact_event_timelines();
    test_fifo_overflow_and_identical_events();
    test_link_timeout_restoration_and_single_alive_event();
    test_zero_counts_do_not_start_link_until_first_transaction();
    test_three_zero_counts_then_first_count_one();
    test_wrap_safe_event_and_transaction_count();
    test_suppression_pauses_events_and_pending_queue();
    test_suppression_pauses_lost_flash();
    test_stage_events_pause_lower_priority_lost_flash();
    test_arguments_and_event_names();
    return 0;
}
