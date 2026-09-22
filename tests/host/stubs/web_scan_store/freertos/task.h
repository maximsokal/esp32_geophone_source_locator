#pragma once

#include "freertos/FreeRTOS.h"

UBaseType_t uxTaskPriorityGet(void *task);
void vTaskPrioritySet(void *task, UBaseType_t priority);
