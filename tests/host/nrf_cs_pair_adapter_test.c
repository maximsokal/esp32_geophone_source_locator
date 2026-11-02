#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "nrf_cs_pair_adapter.h"

enum {
    CALL_CONNECT = 0,
    CALL_SECURE,
    CALL_MTU,
    CALL_DISCOVER,
    CALL_READ_FEATURES,
    CALL_CONFIGURE_RANGING,
    CALL_CAPABILITIES,
    CALL_CONFIG,
    CALL_CS_SECURITY,
    CALL_PROCEDURE,
    CALL_CLEANUP,
    CALL_COUNT
};

typedef struct {
    unsigned calls[CALL_COUNT];
    int fail_call;
    bool realtime;
    unsigned completions;
    nrf_cs_step_t completed_step;
    nrf_cs_measurement_t completed_measurement;
    const nrf_cs_pair_adapter_t *adapter;       /* when set, the timing is read from inside the completion callback */
    bool timing_valid;
    nrf_cs_pair_adapter_timing_t timing;
} fake_backend_t;

static int record(fake_backend_t *fake, int call)
{
    fake->calls[call]++;
    return fake->fail_call == call ? -1 : 0;
}

static int fake_connect(void *context, nrf_cs_node_id_t reflector)
{
    fake_backend_t *fake = context;
    assert(reflector == 9U);
    return record(fake, CALL_CONNECT);
}

static int fake_secure(void *context) { return record(context, CALL_SECURE); }
static int fake_mtu(void *context) { return record(context, CALL_MTU); }
static int fake_discover(void *context) { return record(context, CALL_DISCOVER); }
static int fake_read_features(void *context) { return record(context, CALL_READ_FEATURES); }

static int fake_configure_ranging(void *context, bool realtime)
{
    fake_backend_t *fake = context;
    fake->realtime = realtime;
    return record(fake, CALL_CONFIGURE_RANGING);
}

static int fake_capabilities(void *context) { return record(context, CALL_CAPABILITIES); }
static int fake_config(void *context) { return record(context, CALL_CONFIG); }
static int fake_cs_security(void *context) { return record(context, CALL_CS_SECURITY); }
static int fake_procedure(void *context) { return record(context, CALL_PROCEDURE); }
static int fake_cleanup(void *context) { return record(context, CALL_CLEANUP); }

static void completed(
    const nrf_cs_step_t *step,
    const nrf_cs_measurement_t *measurement,
    void *context)
{
    fake_backend_t *fake = context;
    fake->completions++;
    fake->completed_step = *step;
    fake->completed_measurement = *measurement;
    if (fake->adapter != NULL) {
        fake->timing_valid = nrf_cs_pair_adapter_last_timing(fake->adapter, &fake->timing);
    }
}

static nrf_cs_step_t make_step(uint32_t attempt_id)
{
    nrf_cs_step_t step = {
        .cycle_id = 11U,
        .membership_generation = 4U,
        .attempt_id = attempt_id,
        .pair_index = 2U,
        .attempt_number = 1U,
        .initiator = 3U,
        .reflector = 9U,
        .timeout_ms = 250U,
    };
    return step;
}

static nrf_cs_pair_adapter_t make_adapter(fake_backend_t *fake)
{
    nrf_cs_pair_adapter_t adapter;
    const nrf_cs_pair_adapter_ops_t ops = {
        .connect = fake_connect,
        .secure = fake_secure,
        .exchange_mtu = fake_mtu,
        .discover_ras = fake_discover,
        .read_features = fake_read_features,
        .configure_ranging_data = fake_configure_ranging,
        .exchange_capabilities = fake_capabilities,
        .create_config = fake_config,
        .enable_cs_security = fake_cs_security,
        .start_procedure = fake_procedure,
        .cleanup = fake_cleanup,
        .context = fake,
    };

    assert(nrf_cs_pair_adapter_init(&adapter, &ops) == NRF_CS_PAIR_ADAPTER_OK);
    return adapter;
}

static uint32_t g_now_ms;
static uint32_t fake_now_ms(void *context)
{
    (void)context;
    return g_now_ms;
}

static nrf_cs_pair_adapter_t make_clocked_adapter(fake_backend_t *fake)
{
    nrf_cs_pair_adapter_t adapter;
    const nrf_cs_pair_adapter_ops_t ops = {
        .connect = fake_connect,
        .secure = fake_secure,
        .exchange_mtu = fake_mtu,
        .discover_ras = fake_discover,
        .read_features = fake_read_features,
        .configure_ranging_data = fake_configure_ranging,
        .exchange_capabilities = fake_capabilities,
        .create_config = fake_config,
        .enable_cs_security = fake_cs_security,
        .start_procedure = fake_procedure,
        .cleanup = fake_cleanup,
        .now_ms = fake_now_ms,
        .context = fake,
    };

    assert(nrf_cs_pair_adapter_init(&adapter, &ops) == NRF_CS_PAIR_ADAPTER_OK);
    return adapter;
}

static void drive_to_procedure(
    nrf_cs_pair_adapter_t *adapter,
    nrf_cs_step_t *step,
    bool realtime)
{
    assert(nrf_cs_pair_adapter_connected(adapter, step) == NRF_CS_PAIR_ADAPTER_OK);
    assert(nrf_cs_pair_adapter_security_ready(adapter, step) == NRF_CS_PAIR_ADAPTER_OK);
    assert(nrf_cs_pair_adapter_mtu_ready(adapter, step) == NRF_CS_PAIR_ADAPTER_OK);
    assert(nrf_cs_pair_adapter_discovery_ready(adapter, step) == NRF_CS_PAIR_ADAPTER_OK);
    assert(nrf_cs_pair_adapter_features_ready(adapter, step, realtime) == NRF_CS_PAIR_ADAPTER_OK);
    assert(nrf_cs_pair_adapter_capabilities_ready(adapter, step) == NRF_CS_PAIR_ADAPTER_OK);
    assert(nrf_cs_pair_adapter_config_ready(adapter, step) == NRF_CS_PAIR_ADAPTER_OK);
    assert(nrf_cs_pair_adapter_cs_security_ready(adapter, step) == NRF_CS_PAIR_ADAPTER_OK);
    assert(nrf_cs_pair_adapter_phase(adapter) == NRF_CS_PAIR_PHASE_PROCEDURE);
}

static void test_success_path_is_bounded_and_ordered(void)
{
    fake_backend_t fake = {0};
    nrf_cs_pair_adapter_t adapter;
    nrf_cs_step_t step = make_step(7U);
    nrf_cs_measurement_t measurement = {
        .outcome = NRF_CS_OUTCOME_SUCCESS,
        .distance_mm = 1234,
        .confidence = 1000U,
        .provider_quality_flags = 0x5U,
    };

    fake.fail_call = -1;
    adapter = make_adapter(&fake);

    assert(nrf_cs_pair_adapter_execute(&adapter, 3U, &step, completed, &fake) ==
           NRF_CS_PAIR_ADAPTER_OK);
    drive_to_procedure(&adapter, &step, true);
    assert(fake.realtime);
    assert(nrf_cs_pair_adapter_measurement_ready(&adapter, &step, &measurement) ==
           NRF_CS_PAIR_ADAPTER_OK);
    assert(nrf_cs_pair_adapter_phase(&adapter) == NRF_CS_PAIR_PHASE_CLEANUP);
    assert(fake.calls[CALL_CLEANUP] == 1U);
    assert(fake.completions == 0U);

    assert(nrf_cs_pair_adapter_cleanup_complete(&adapter, &step) ==
           NRF_CS_PAIR_ADAPTER_COMPLETED);
    assert(fake.completions == 1U);
    assert(fake.completed_step.attempt_id == 7U);
    assert(fake.completed_measurement.distance_mm == 1234);
    assert(!nrf_cs_pair_adapter_is_active(&adapter));

    for (int i = CALL_CONNECT; i <= CALL_CLEANUP; ++i) {
        assert(fake.calls[i] == 1U);
    }
}

static void test_on_demand_path_is_selected_from_feature_result(void)
{
    fake_backend_t fake = {0};
    nrf_cs_pair_adapter_t adapter;
    nrf_cs_step_t step = make_step(8U);

    fake.fail_call = -1;
    adapter = make_adapter(&fake);
    assert(nrf_cs_pair_adapter_execute(&adapter, 3U, &step, completed, &fake) ==
           NRF_CS_PAIR_ADAPTER_OK);
    drive_to_procedure(&adapter, &step, false);
    assert(!fake.realtime);
}

static void test_timeout_becomes_scheduler_timeout_only_after_cleanup(void)
{
    fake_backend_t fake = {0};
    nrf_cs_pair_adapter_t adapter;
    nrf_cs_step_t step = make_step(9U);

    fake.fail_call = -1;
    adapter = make_adapter(&fake);
    assert(nrf_cs_pair_adapter_execute(&adapter, 3U, &step, completed, &fake) ==
           NRF_CS_PAIR_ADAPTER_OK);
    assert(nrf_cs_pair_adapter_timeout(&adapter, &step) == NRF_CS_PAIR_ADAPTER_OK);
    assert(fake.completions == 0U);
    assert(nrf_cs_pair_adapter_cleanup_complete(&adapter, &step) ==
           NRF_CS_PAIR_ADAPTER_COMPLETED);
    assert(fake.completed_measurement.outcome == NRF_CS_OUTCOME_TIMEOUT);
}

static void test_backend_failure_maps_to_transport_error(void)
{
    fake_backend_t fake = {0};
    nrf_cs_pair_adapter_t adapter;
    nrf_cs_step_t step = make_step(10U);

    fake.fail_call = CALL_MTU;
    adapter = make_adapter(&fake);
    assert(nrf_cs_pair_adapter_execute(&adapter, 3U, &step, completed, &fake) ==
           NRF_CS_PAIR_ADAPTER_OK);
    assert(nrf_cs_pair_adapter_connected(&adapter, &step) == NRF_CS_PAIR_ADAPTER_OK);
    assert(nrf_cs_pair_adapter_security_ready(&adapter, &step) == NRF_CS_PAIR_ADAPTER_OK);
    assert(nrf_cs_pair_adapter_phase(&adapter) == NRF_CS_PAIR_PHASE_CLEANUP);
    assert(nrf_cs_pair_adapter_cleanup_complete(&adapter, &step) ==
           NRF_CS_PAIR_ADAPTER_COMPLETED);
    assert(fake.completed_measurement.outcome == NRF_CS_OUTCOME_TRANSPORT_ERROR);
}

static void test_stale_tokens_and_wrong_local_initiator_are_rejected(void)
{
    fake_backend_t fake = {0};
    nrf_cs_pair_adapter_t adapter;
    nrf_cs_step_t step = make_step(11U);
    nrf_cs_step_t stale = step;

    fake.fail_call = -1;
    adapter = make_adapter(&fake);
    assert(nrf_cs_pair_adapter_execute(&adapter, 4U, &step, completed, &fake) ==
           NRF_CS_PAIR_ADAPTER_ERR_ARGUMENT);
    assert(!nrf_cs_pair_adapter_is_active(&adapter));

    assert(nrf_cs_pair_adapter_execute(&adapter, 3U, &step, completed, &fake) ==
           NRF_CS_PAIR_ADAPTER_OK);
    stale.attempt_id++;
    assert(nrf_cs_pair_adapter_connected(&adapter, &stale) ==
           NRF_CS_PAIR_ADAPTER_ERR_STALE);
    assert(nrf_cs_pair_adapter_phase(&adapter) == NRF_CS_PAIR_PHASE_CONNECTING);
}

static void test_only_one_attempt_can_be_active(void)
{
    fake_backend_t fake = {0};
    nrf_cs_pair_adapter_t adapter;
    nrf_cs_step_t first = make_step(12U);
    nrf_cs_step_t second = make_step(13U);

    fake.fail_call = -1;
    adapter = make_adapter(&fake);
    assert(nrf_cs_pair_adapter_execute(&adapter, 3U, &first, completed, &fake) ==
           NRF_CS_PAIR_ADAPTER_OK);
    assert(nrf_cs_pair_adapter_execute(&adapter, 3U, &second, completed, &fake) ==
           NRF_CS_PAIR_ADAPTER_ERR_BUSY);
}

/* CS-TIME-01. Bench 2026-11-02, MAIN attempt 2 (budget 2400 ms): the distance was ready at +1268 ms, the budget timer fired
 * at +2400 ms while the BLE disconnect was still running, and the disconnect finished at +5262 ms. The log said
 * "TIMEOUT phase=CLEANUP" and then "result=SUCCESS +5262ms" without telling the measurement from the cleanup. */
static void test_a_late_cleanup_is_reported_apart_from_the_measurement(void)
{
    fake_backend_t fake = {0};
    nrf_cs_pair_adapter_t adapter;
    nrf_cs_step_t step = make_step(21U);
    nrf_cs_measurement_t measurement = {
        .outcome = NRF_CS_OUTCOME_SUCCESS, .distance_mm = 6194, .confidence = 755U, .provider_quality_flags = 0U,
    };

    step.timeout_ms = 2400U;
    fake.fail_call = -1;
    adapter = make_clocked_adapter(&fake);
    fake.adapter = &adapter;
    g_now_ms = 153652U;
    assert(nrf_cs_pair_adapter_execute(&adapter, 3U, &step, completed, &fake) == NRF_CS_PAIR_ADAPTER_OK);
    drive_to_procedure(&adapter, &step, true);
    g_now_ms = 153652U + 1268U;
    assert(nrf_cs_pair_adapter_measurement_ready(&adapter, &step, &measurement) == NRF_CS_PAIR_ADAPTER_OK);
    assert(nrf_cs_pair_adapter_phase(&adapter) == NRF_CS_PAIR_PHASE_CLEANUP);

    /* The budget timer fires during cleanup: refused, the outcome that is already known stays. */
    g_now_ms = 153652U + 2400U;
    assert(nrf_cs_pair_adapter_timeout(&adapter, &step) == NRF_CS_PAIR_ADAPTER_ERR_STATE);
    assert(nrf_cs_pair_adapter_phase(&adapter) == NRF_CS_PAIR_PHASE_CLEANUP);
    assert(fake.completions == 0U);

    g_now_ms = 153652U + 5262U;
    assert(nrf_cs_pair_adapter_cleanup_complete(&adapter, &step) == NRF_CS_PAIR_ADAPTER_COMPLETED);
    assert(fake.completions == 1U);
    assert(fake.completed_measurement.outcome == NRF_CS_OUTCOME_SUCCESS && fake.completed_measurement.distance_mm == 6194);
    assert(fake.timing_valid);                                   /* readable from inside the callback */
    assert(fake.timing.measured_ms == 1268U);
    assert(fake.timing.cleanup_ms == 3994U);
    assert(fake.timing.total_ms == 5262U);
    assert(fake.timing.budget_exceeded);
    assert(fake.timing.timeout_during_cleanup);

    /* And afterwards too, until the next attempt overwrites it. */
    nrf_cs_pair_adapter_timing_t later;
    assert(nrf_cs_pair_adapter_last_timing(&adapter, &later) && later.cleanup_ms == 3994U);
}

static void test_a_prompt_cleanup_is_not_flagged_and_the_next_attempt_starts_clean(void)
{
    fake_backend_t fake = {0};
    nrf_cs_pair_adapter_t adapter;
    nrf_cs_step_t step = make_step(22U);
    nrf_cs_measurement_t measurement = {
        .outcome = NRF_CS_OUTCOME_SUCCESS, .distance_mm = 6272, .confidence = 953U, .provider_quality_flags = 0U,
    };

    step.timeout_ms = 2400U;
    fake.fail_call = -1;
    adapter = make_clocked_adapter(&fake);
    fake.adapter = &adapter;

    /* First attempt: late cleanup, flagged. */
    g_now_ms = 1000U;
    assert(nrf_cs_pair_adapter_execute(&adapter, 3U, &step, completed, &fake) == NRF_CS_PAIR_ADAPTER_OK);
    drive_to_procedure(&adapter, &step, true);
    g_now_ms = 2268U;
    assert(nrf_cs_pair_adapter_measurement_ready(&adapter, &step, &measurement) == NRF_CS_PAIR_ADAPTER_OK);
    g_now_ms = 3400U;
    assert(nrf_cs_pair_adapter_timeout(&adapter, &step) == NRF_CS_PAIR_ADAPTER_ERR_STATE);
    g_now_ms = 6262U;
    assert(nrf_cs_pair_adapter_cleanup_complete(&adapter, &step) == NRF_CS_PAIR_ADAPTER_COMPLETED);
    assert(fake.timing.timeout_during_cleanup && fake.timing.budget_exceeded);

    /* Second attempt (attempt 1 of the same log: distance at +1428, disconnected at +1481): nothing carried over. */
    step = make_step(23U);
    step.timeout_ms = 2400U;
    g_now_ms = 10000U;
    assert(nrf_cs_pair_adapter_execute(&adapter, 3U, &step, completed, &fake) == NRF_CS_PAIR_ADAPTER_OK);
    drive_to_procedure(&adapter, &step, true);
    g_now_ms = 11428U;
    assert(nrf_cs_pair_adapter_measurement_ready(&adapter, &step, &measurement) == NRF_CS_PAIR_ADAPTER_OK);
    g_now_ms = 11481U;
    assert(nrf_cs_pair_adapter_cleanup_complete(&adapter, &step) == NRF_CS_PAIR_ADAPTER_COMPLETED);
    assert(fake.timing_valid && fake.timing.measured_ms == 1428U && fake.timing.cleanup_ms == 53U);
    assert(fake.timing.total_ms == 1481U);
    assert(!fake.timing.budget_exceeded && !fake.timing.timeout_during_cleanup);
}

static void test_a_timeout_while_measuring_is_a_timeout_and_the_budget_edge_is_exact(void)
{
    fake_backend_t fake = {0};
    nrf_cs_pair_adapter_t adapter;
    nrf_cs_step_t step = make_step(24U);

    step.timeout_ms = 2400U;
    fake.fail_call = -1;
    adapter = make_clocked_adapter(&fake);
    fake.adapter = &adapter;

    /* The timer fires while the procedure runs: that is a real TIMEOUT, cleanup is not the cause. */
    g_now_ms = 500U;
    assert(nrf_cs_pair_adapter_execute(&adapter, 3U, &step, completed, &fake) == NRF_CS_PAIR_ADAPTER_OK);
    drive_to_procedure(&adapter, &step, true);
    g_now_ms = 500U + 2400U;
    assert(nrf_cs_pair_adapter_timeout(&adapter, &step) == NRF_CS_PAIR_ADAPTER_OK);
    g_now_ms = 500U + 2450U;
    assert(nrf_cs_pair_adapter_cleanup_complete(&adapter, &step) == NRF_CS_PAIR_ADAPTER_COMPLETED);
    assert(fake.completed_measurement.outcome == NRF_CS_OUTCOME_TIMEOUT);
    assert(fake.timing.measured_ms == 2400U && fake.timing.cleanup_ms == 50U);
    assert(fake.timing.budget_exceeded && !fake.timing.timeout_during_cleanup);

    /* total == budget is within it, one millisecond more is not. */
    for (uint32_t extra = 0U; extra <= 1U; ++extra) {
        nrf_cs_measurement_t ok = { .outcome = NRF_CS_OUTCOME_SUCCESS, .distance_mm = 1000, .confidence = 900U };
        step = make_step(25U + extra);
        step.timeout_ms = 2400U;
        g_now_ms = 0U;
        assert(nrf_cs_pair_adapter_execute(&adapter, 3U, &step, completed, &fake) == NRF_CS_PAIR_ADAPTER_OK);
        drive_to_procedure(&adapter, &step, true);
        g_now_ms = 2000U;
        assert(nrf_cs_pair_adapter_measurement_ready(&adapter, &step, &ok) == NRF_CS_PAIR_ADAPTER_OK);
        g_now_ms = 2400U + extra;
        assert(nrf_cs_pair_adapter_cleanup_complete(&adapter, &step) == NRF_CS_PAIR_ADAPTER_COMPLETED);
        assert(fake.timing.total_ms == 2400U + extra);
        assert(fake.timing.budget_exceeded == (extra == 1U));
    }
}

static void test_timing_survives_the_clock_wrap_and_is_absent_without_a_clock(void)
{
    fake_backend_t fake = {0};
    nrf_cs_pair_adapter_t adapter;
    nrf_cs_step_t step = make_step(30U);
    nrf_cs_measurement_t ok = { .outcome = NRF_CS_OUTCOME_SUCCESS, .distance_mm = 1000, .confidence = 900U };

    fake.fail_call = -1;
    adapter = make_clocked_adapter(&fake);
    fake.adapter = &adapter;
    g_now_ms = 0xFFFFFF00U;                                      /* uptime counter about to wrap */
    assert(nrf_cs_pair_adapter_execute(&adapter, 3U, &step, completed, &fake) == NRF_CS_PAIR_ADAPTER_OK);
    drive_to_procedure(&adapter, &step, true);
    g_now_ms = 0x00000100U;
    assert(nrf_cs_pair_adapter_measurement_ready(&adapter, &step, &ok) == NRF_CS_PAIR_ADAPTER_OK);
    g_now_ms = 0x00000140U;
    assert(nrf_cs_pair_adapter_cleanup_complete(&adapter, &step) == NRF_CS_PAIR_ADAPTER_COMPLETED);
    assert(fake.timing.measured_ms == 0x200U && fake.timing.cleanup_ms == 0x40U && fake.timing.total_ms == 0x240U);

    /* An adapter without a clock completes as before and reports no timing. */
    fake_backend_t plain = {0};
    nrf_cs_pair_adapter_t plain_adapter;
    nrf_cs_step_t plain_step = make_step(31U);
    nrf_cs_pair_adapter_timing_t out;
    plain.fail_call = -1;
    plain_adapter = make_adapter(&plain);
    assert(!nrf_cs_pair_adapter_last_timing(&plain_adapter, &out));          /* nothing finished yet */
    assert(nrf_cs_pair_adapter_execute(&plain_adapter, 3U, &plain_step, completed, &plain) == NRF_CS_PAIR_ADAPTER_OK);
    drive_to_procedure(&plain_adapter, &plain_step, true);
    assert(nrf_cs_pair_adapter_measurement_ready(&plain_adapter, &plain_step, &ok) == NRF_CS_PAIR_ADAPTER_OK);
    assert(nrf_cs_pair_adapter_cleanup_complete(&plain_adapter, &plain_step) == NRF_CS_PAIR_ADAPTER_COMPLETED);
    assert(plain.completions == 1U);
    assert(!nrf_cs_pair_adapter_last_timing(&plain_adapter, &out) && !out.valid);
    assert(!nrf_cs_pair_adapter_last_timing(NULL, &out) && !nrf_cs_pair_adapter_last_timing(&plain_adapter, NULL));
}

/* A pairing that fails (bench: TRANSPORT_ERROR status=9 in SECURING, 4 of 24 attempts) ends at once: the measured time is
 * the time to the failure and nothing is flagged as a late cleanup. */
static void test_a_failure_in_securing_has_its_own_short_timing(void)
{
    fake_backend_t fake = {0};
    nrf_cs_pair_adapter_t adapter;
    nrf_cs_step_t step = make_step(32U);

    step.timeout_ms = 2400U;
    fake.fail_call = -1;
    adapter = make_clocked_adapter(&fake);
    fake.adapter = &adapter;
    g_now_ms = 159107U;
    assert(nrf_cs_pair_adapter_execute(&adapter, 3U, &step, completed, &fake) == NRF_CS_PAIR_ADAPTER_OK);
    g_now_ms = 159107U + 131U;
    assert(nrf_cs_pair_adapter_connected(&adapter, &step) == NRF_CS_PAIR_ADAPTER_OK);
    assert(nrf_cs_pair_adapter_phase(&adapter) == NRF_CS_PAIR_PHASE_SECURING);
    g_now_ms = 159107U + 233U;
    assert(nrf_cs_pair_adapter_transport_error(&adapter, &step) == NRF_CS_PAIR_ADAPTER_OK);
    assert(nrf_cs_pair_adapter_cleanup_complete(&adapter, &step) == NRF_CS_PAIR_ADAPTER_COMPLETED);
    assert(fake.completed_measurement.outcome == NRF_CS_OUTCOME_TRANSPORT_ERROR);
    assert(fake.timing.measured_ms == 233U && fake.timing.cleanup_ms == 0U);
    assert(!fake.timing.budget_exceeded && !fake.timing.timeout_during_cleanup);
}

int main(void)
{
    test_success_path_is_bounded_and_ordered();
    test_on_demand_path_is_selected_from_feature_result();
    test_timeout_becomes_scheduler_timeout_only_after_cleanup();
    test_backend_failure_maps_to_transport_error();
    test_stale_tokens_and_wrong_local_initiator_are_rejected();
    test_only_one_attempt_can_be_active();
    test_a_late_cleanup_is_reported_apart_from_the_measurement();
    test_a_prompt_cleanup_is_not_flagged_and_the_next_attempt_starts_clean();
    test_a_timeout_while_measuring_is_a_timeout_and_the_budget_edge_is_exact();
    test_timing_survives_the_clock_wrap_and_is_absent_without_a_clock();
    test_a_failure_in_securing_has_its_own_short_timing();
    return 0;
}
