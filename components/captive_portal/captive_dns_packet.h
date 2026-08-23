#pragma once

#include <stddef.h>
#include <stdint.h>


/*
 * Build a DNS response for one incoming DNS request.
 *
 * Type A requests are redirected to ap_ip_addr.
 * Unsupported record types receive a valid response
 * with no answer records.
 *
 * Returns:
 *   > 0  response length
 *   < 0  malformed or unsupported request
 */
int captive_dns_packet_build_response(
    const uint8_t *request,
    size_t request_length,
    uint32_t ap_ip_addr,
    uint8_t *response,
    size_t response_capacity
);