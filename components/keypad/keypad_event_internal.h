#pragma once

#include <stdbool.h>

#include "keypad.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


#define KEYPAD_POLL_PERIOD_MS    20
#define KEYPAD_DEBOUNCE_COUNT     3


typedef struct
{
    TaskHandle_t task;

    keypad_event_callback_t callback;

    void *user_context;

    volatile bool running;

} keypad_event_state_t;


keypad_event_state_t *keypad_event_state(void);


void keypad_event_worker(
    void *arg
);
