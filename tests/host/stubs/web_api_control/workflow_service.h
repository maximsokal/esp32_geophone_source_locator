#pragma once

#include "../../../../components/workflow_service/include/workflow_service.h"

esp_err_t workflow_service_capture_height_references(
    uint8_t *requested_channel_mask,
    uint16_t *requested_generation
);
