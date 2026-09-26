#pragma once

#include <stdint.h>

#include "esp_err.h"

#define NRF_TOPOLOGY_REMOTE_COUNT 8U

esp_err_t nrf_topology_service_request_height_reference_capture(
    uint8_t *requested_channel_mask,
    uint16_t *requested_generation);
