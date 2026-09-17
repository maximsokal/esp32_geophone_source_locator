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

int main(void)
{
    test_success_path_is_bounded_and_ordered();
    test_on_demand_path_is_selected_from_feature_result();
    test_timeout_becomes_scheduler_timeout_only_after_cleanup();
    test_backend_failure_maps_to_transport_error();
    test_stale_tokens_and_wrong_local_initiator_are_rejected();
    test_only_one_attempt_can_be_active();
    return 0;
}
