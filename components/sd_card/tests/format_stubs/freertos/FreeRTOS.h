#pragma once

typedef unsigned int TickType_t;
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
typedef struct { int unused; } portMUX_TYPE;

#define portMUX_INITIALIZER_UNLOCKED {0}
#define taskENTER_CRITICAL(lock) ((void)(lock))
#define taskEXIT_CRITICAL(lock) ((void)(lock))
#define pdPASS 1
#define pdMS_TO_TICKS(milliseconds) ((TickType_t)(milliseconds))
