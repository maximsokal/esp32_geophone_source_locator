#pragma once

#include "FreeRTOS.h"
typedef void *TimerHandle_t;
typedef void (*TimerCallbackFunction_t)(TimerHandle_t);
TimerHandle_t xTimerCreate(const char *name, TickType_t period, BaseType_t reload,
    void *id, TimerCallbackFunction_t callback);
BaseType_t xTimerStart(TimerHandle_t timer, TickType_t wait);
BaseType_t xTimerDelete(TimerHandle_t timer, TickType_t wait);
