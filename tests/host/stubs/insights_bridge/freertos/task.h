#pragma once
#include <stdint.h>
#include "freertos/FreeRTOS.h"
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task, const char *name, uint32_t stack, void *arg, UBaseType_t priority, TaskHandle_t *handle, BaseType_t core);
BaseType_t xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t wait_ticks);
