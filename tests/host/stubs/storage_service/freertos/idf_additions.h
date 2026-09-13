#pragma once
#include <stdint.h>
#include "freertos/FreeRTOS.h"
BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t task, const char *name, uint32_t stack, void *arg, UBaseType_t priority, TaskHandle_t *handle, BaseType_t core, uint32_t caps);
void vTaskDeleteWithCaps(TaskHandle_t task);
