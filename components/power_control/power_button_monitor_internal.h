#pragma once

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"


#define BUTTON_POLL_MS                    10
#define BUTTON_PRESS_CONFIRM_MS          150
#define BUTTON_RELEASE_CONFIRM_MS         50

#define POWER_EVENT_SHUTDOWN_REQUESTED  BIT0


typedef struct
{
    EventGroupHandle_t events;

    TaskHandle_t task;

    volatile bool running;

    bool initialized;

} power_button_monitor_state_t;


power_button_monitor_state_t *
power_button_monitor_state(void);


unsigned power_button_required_samples(
    unsigned duration_ms
);


bool power_button_wait_for_level(
    bool high,
    unsigned duration_ms,
    bool stop_sensitive
);


void power_button_monitor_worker(
    void *argument
);
