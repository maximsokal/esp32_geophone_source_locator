#pragma once

#include <stddef.h>

#include "freertos/FreeRTOS.h"

typedef struct fake_queue *QueueHandle_t;

QueueHandle_t xQueueCreate(unsigned length, unsigned item_size);
BaseType_t xQueueReset(QueueHandle_t queue);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait_ticks);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait_ticks);
