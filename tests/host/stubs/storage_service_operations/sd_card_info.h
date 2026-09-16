#pragma once

#include "esp_err.h"
#include "storage_service_internal.h"

esp_err_t sd_card_get_info(sd_card_info_t *info);
