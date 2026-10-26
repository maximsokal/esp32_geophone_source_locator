#include "nrf_node_link_status_policy.h"

#include "nrf_upstream_protocol.h"

nrf_node_link_status_t nrf_node_link_status_evaluate(
    const nrf_node_link_status_input_t *input,
    uint64_t now_ms,
    bool recording)
{
    /*
     * input->result is the outcome of the last request the node saw, which the
     * nRF keeps in STATUS until the next ASSIGN. It says nothing about whether
     * the link is up, so a stray INVALID_FRAME must not hide a working node.
     */
    if (input == NULL || !input->assigned || !input->status_valid ||
        !input->status_identity_matches || now_ms < input->status_receipt_uptime_ms ||
        now_ms - input->status_receipt_uptime_ms >= NRF_NODE_LINK_STATUS_STALE_LIMIT_MS ||
        (input->flags & NRF_UPSTREAM_STATUS_FLAG_ASSIGNED) == 0U ||
        (input->lifecycle != NRF_UPSTREAM_LIFECYCLE_BOOTING &&
         input->lifecycle != NRF_UPSTREAM_LIFECYCLE_READY)) {
        return NRF_NODE_LINK_STATUS_NOT_CONNECTED;
    }

    if (input->lifecycle != NRF_UPSTREAM_LIFECYCLE_READY ||
        (input->flags & NRF_UPSTREAM_STATUS_FLAG_TARGET_REGISTERED) == 0U ||
        (input->sensor_result_valid &&
         input->sensor_result != NRF_UPSTREAM_SENSOR_RESULT_OK &&
         input->sensor_result != NRF_UPSTREAM_SENSOR_RESULT_PARTIAL)) {
        return NRF_NODE_LINK_STATUS_CONNECTED;
    }

    return recording
        ? NRF_NODE_LINK_STATUS_MEASURING
        : NRF_NODE_LINK_STATUS_READY;
}

const char *nrf_node_link_status_name(nrf_node_link_status_t status)
{
    switch (status) {
    case NRF_NODE_LINK_STATUS_NOT_CONNECTED:
        return "NOT_CONNECTED";
    case NRF_NODE_LINK_STATUS_CONNECTED:
        return "CONNECTED";
    case NRF_NODE_LINK_STATUS_READY:
        return "READY";
    case NRF_NODE_LINK_STATUS_MEASURING:
        return "MEASURING";
    default:
        return "NOT_CONNECTED";
    }
}
