#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "nrf_node_link_status_policy.h"
#include "nrf_upstream_protocol.h"

static nrf_node_link_status_input_t ready_status(void)
{
    return (nrf_node_link_status_input_t) {
        .assigned = true,
        .status_valid = true,
        .status_identity_matches = true,
        .lifecycle = NRF_UPSTREAM_LIFECYCLE_READY,
        .result = NRF_UPSTREAM_RESULT_OK,
        .flags = NRF_UPSTREAM_STATUS_FLAG_ASSIGNED |
            NRF_UPSTREAM_STATUS_FLAG_TARGET_REGISTERED,
        .status_receipt_uptime_ms = 1000U,
    };
}

static void test_status_not_connected_when_node_absent_unassigned_or_error(void)
{
    nrf_node_link_status_input_t input = ready_status();
    input.assigned = false;
    assert(nrf_node_link_status_evaluate(&input, 1001U, false) ==
        NRF_NODE_LINK_STATUS_NOT_CONNECTED);

    input = ready_status();
    input.flags = NRF_UPSTREAM_STATUS_FLAG_TARGET_REGISTERED;
    assert(nrf_node_link_status_evaluate(&input, 1001U, false) ==
        NRF_NODE_LINK_STATUS_NOT_CONNECTED);

    input = ready_status();
    input.result = NRF_UPSTREAM_RESULT_INTERNAL_ERROR;
    assert(nrf_node_link_status_evaluate(&input, 1001U, false) ==
        NRF_NODE_LINK_STATUS_NOT_CONNECTED);

    input = ready_status();
    input.lifecycle = NRF_UPSTREAM_LIFECYCLE_UNAVAILABLE;
    assert(nrf_node_link_status_evaluate(&input, 1001U, false) ==
        NRF_NODE_LINK_STATUS_NOT_CONNECTED);
}

static void test_status_connected_when_fresh_status_not_ready(void)
{
    nrf_node_link_status_input_t input = ready_status();
    input.lifecycle = NRF_UPSTREAM_LIFECYCLE_BOOTING;
    assert(nrf_node_link_status_evaluate(&input, 1001U, false) ==
        NRF_NODE_LINK_STATUS_CONNECTED);

    input = ready_status();
    input.flags = NRF_UPSTREAM_STATUS_FLAG_ASSIGNED;
    assert(nrf_node_link_status_evaluate(&input, 1001U, false) ==
        NRF_NODE_LINK_STATUS_CONNECTED);

    input = ready_status();
    input.sensor_result_valid = true;
    input.sensor_result = NRF_UPSTREAM_SENSOR_RESULT_INTERNAL_ERROR;
    assert(nrf_node_link_status_evaluate(&input, 1001U, false) ==
        NRF_NODE_LINK_STATUS_CONNECTED);
}

static void test_status_ready_when_fresh_ready_ok_assigned_registered(void)
{
    const nrf_node_link_status_input_t input = ready_status();
    assert(nrf_node_link_status_evaluate(&input, 1001U, false) ==
        NRF_NODE_LINK_STATUS_READY);
}

static void test_status_measuring_when_ready_and_recording(void)
{
    const nrf_node_link_status_input_t input = ready_status();
    assert(nrf_node_link_status_evaluate(&input, 1001U, true) ==
        NRF_NODE_LINK_STATUS_MEASURING);
}

static void test_stale_limit_includes_exact_boundary(void)
{
    nrf_node_link_status_input_t input = ready_status();
    assert(nrf_node_link_status_evaluate(
               &input,
               input.status_receipt_uptime_ms + NRF_NODE_LINK_STATUS_STALE_LIMIT_MS - 1U,
               false) == NRF_NODE_LINK_STATUS_READY);
    assert(nrf_node_link_status_evaluate(
               &input,
               input.status_receipt_uptime_ms + NRF_NODE_LINK_STATUS_STALE_LIMIT_MS,
               false) == NRF_NODE_LINK_STATUS_NOT_CONNECTED);
}

static void test_busy_window_keeps_last_good_status_until_limit(void)
{
    nrf_node_link_status_input_t cached_status = ready_status();
    uint64_t fake_now_ms = cached_status.status_receipt_uptime_ms;

    /* Busy refreshes leave this cached STATUS untouched until its age expires. */
    fake_now_ms += NRF_NODE_LINK_STATUS_STALE_LIMIT_MS - 1U;
    assert(nrf_node_link_status_evaluate(&cached_status, fake_now_ms, false) ==
        NRF_NODE_LINK_STATUS_READY);
    fake_now_ms += 1U;
    assert(nrf_node_link_status_evaluate(&cached_status, fake_now_ms, false) ==
        NRF_NODE_LINK_STATUS_NOT_CONNECTED);
}

static void test_identity_mismatch_is_not_connected(void)
{
    nrf_node_link_status_input_t input = ready_status();
    input.status_identity_matches = false;
    assert(nrf_node_link_status_evaluate(&input, 1001U, false) ==
        NRF_NODE_LINK_STATUS_NOT_CONNECTED);
}

static void test_refresh_and_stale_tolerances_are_pinned(void)
{
    assert(NRF_SENSOR_REFRESH_PERIOD_MS == 2000U);
    assert(NRF_NODE_LINK_STATUS_STALE_PERIODS == 3U);
    assert(NRF_NODE_LINK_STATUS_STALE_LIMIT_MS == 6000U);
}

int main(void)
{
    test_status_not_connected_when_node_absent_unassigned_or_error();
    test_status_connected_when_fresh_status_not_ready();
    test_status_ready_when_fresh_ready_ok_assigned_registered();
    test_status_measuring_when_ready_and_recording();
    test_stale_limit_includes_exact_boundary();
    test_busy_window_keeps_last_good_status_until_limit();
    test_identity_mismatch_is_not_connected();
    test_refresh_and_stale_tolerances_are_pinned();
    puts("nrf_node_link_status_policy_test: PASS");
    return 0;
}
