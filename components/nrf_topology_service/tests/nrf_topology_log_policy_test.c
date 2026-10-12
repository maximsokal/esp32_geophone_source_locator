#include "nrf_topology_log_policy.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>


static void baseline(
    nrf_topology_log_state_t *state,
    uint32_t now_ms,
    uint32_t generation,
    bool tca_available,
    bool tca_fault,
    uint32_t assigned,
    uint32_t conflicts,
    int32_t main_state
)
{
    memset(state, 0, sizeof(*state));
    assert(nrf_topology_log_should_emit(
        state, now_ms, generation, tca_available, tca_fault,
        assigned, conflicts, main_state
    ));
}


static void test_first_identical_generation_and_heartbeat(void)
{
    nrf_topology_log_state_t state = {0};

    assert(nrf_topology_log_should_emit(
        &state, 0U, 1U, true, false, 4U, 0U, 2
    ));
    assert(!nrf_topology_log_should_emit(
        &state, 10000U, 2U, true, false, 4U, 0U, 2
    ));
    assert(!nrf_topology_log_should_emit(
        &state, 59999U, 3U, true, false, 4U, 0U, 2
    ));
    assert(nrf_topology_log_should_emit(
        &state, 60000U, 4U, true, false, 4U, 0U, 2
    ));
    assert(!nrf_topology_log_should_emit(
        &state, 119999U, 4U, true, false, 4U, 0U, 2
    ));
    assert(nrf_topology_log_should_emit(
        &state, 120000U, 4U, true, false, 4U, 0U, 2
    ));
}


static void test_each_reported_field_logs_immediately(void)
{
    unsigned field;

    for (field = 0U; field < 5U; ++field) {
        nrf_topology_log_state_t state = {0};
        bool tca_available = true;
        bool tca_fault = false;
        uint32_t assigned = 4U;
        uint32_t conflicts = 0U;
        int32_t main_state = 2;

        baseline(&state, 10U, 1U, tca_available, tca_fault,
                 assigned, conflicts, main_state);
        if (field == 0U) {
            tca_available = false;
        } else if (field == 1U) {
            tca_fault = true;
        } else if (field == 2U) {
            assigned++;
        } else if (field == 3U) {
            conflicts++;
        } else {
            main_state++;
        }
        assert(nrf_topology_log_should_emit(
            &state, 11U, 1U, tca_available, tca_fault,
            assigned, conflicts, main_state
        ));
    }
}


static void test_heartbeat_handles_uint32_wraparound(void)
{
    nrf_topology_log_state_t state = {0};
    const uint32_t start_ms = UINT32_MAX - 1000U;

    baseline(&state, start_ms, 1U, true, false, 4U, 0U, 2);
    assert(!nrf_topology_log_should_emit(
        &state, (uint32_t)(start_ms + 59999U), 1U,
        true, false, 4U, 0U, 2
    ));
    assert(nrf_topology_log_should_emit(
        &state, (uint32_t)(start_ms + 60000U), 1U,
        true, false, 4U, 0U, 2
    ));
}

static void test_tca_fault_set_clear_and_busy_reasons_emit_once(void)
{
    nrf_topology_tca_log_state_t state = {0};
    assert(nrf_topology_tca_log_update(
        &state, true, false, "verify_timeout") == NRF_TOPOLOGY_TCA_LOG_SET);
    assert(strcmp(state.last_reason, "verify_timeout") == 0);
    assert(state.latched_count == 1U);
    assert(nrf_topology_tca_log_update(
        &state, true, false, "verify_timeout") == NRF_TOPOLOGY_TCA_LOG_NONE);

    assert(nrf_topology_tca_log_update(
        &state, true, true, "mutex_busy") == NRF_TOPOLOGY_TCA_LOG_BUSY);
    assert(strcmp(state.last_reason, "mutex_busy") == 0);
    assert(nrf_topology_tca_log_update(
        &state, true, true, "mutex_busy") == NRF_TOPOLOGY_TCA_LOG_NONE);

    assert(nrf_topology_tca_log_update(
        &state, false, false, "successful_close_verify") == NRF_TOPOLOGY_TCA_LOG_CLEARED);
    assert(strcmp(state.last_reason, "successful_close_verify") == 0);
    assert(state.auto_cleared_count == 1U);
    assert(nrf_topology_tca_log_update(
        &state, false, false, "successful_close_verify") == NRF_TOPOLOGY_TCA_LOG_NONE);

    assert(nrf_topology_tca_log_update(
        &state, true, false, "readback_mismatch") == NRF_TOPOLOGY_TCA_LOG_SET);
    assert(strcmp(state.last_reason, "readback_mismatch") == 0);
    assert(state.latched_count == 2U);
    assert(nrf_topology_tca_log_update(
        &state, false, false, "successful_close_verify") == NRF_TOPOLOGY_TCA_LOG_CLEARED);
}


int main(void)
{
    test_first_identical_generation_and_heartbeat();
    test_each_reported_field_logs_immediately();
    test_heartbeat_handles_uint32_wraparound();
    test_tca_fault_set_clear_and_busy_reasons_emit_once();
    return 0;
}
