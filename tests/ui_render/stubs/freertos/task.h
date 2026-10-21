#pragma once
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
BaseType_t xTaskCreatePinnedToCore(void (*task)(void *), const char *name, uint32_t stack,
    void *arg, unsigned priority, TaskHandle_t *handle, BaseType_t core);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t timeout);
void xTaskNotifyGive(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);
#define taskYIELD() ((void)0)
