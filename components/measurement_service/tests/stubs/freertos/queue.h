#pragma once

#include "freertos/FreeRTOS.h"

QueueHandle_t xQueueCreate(size_t length, size_t item_size);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t timeout);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t timeout);
void vQueueDelete(QueueHandle_t queue);
