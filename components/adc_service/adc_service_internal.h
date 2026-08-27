#pragma once

#include <stdbool.h>

#include "esp_adc/adc_oneshot.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"


extern adc_oneshot_unit_handle_t g_adc_service_handle;
extern SemaphoreHandle_t g_adc_service_mutex;
extern bool g_adc_service_initialized;
