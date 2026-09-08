#pragma once

#include "freertos/FreeRTOS.h"

EventBits_t xEventGroupWaitBits(
    EventGroupHandle_t event_group,
    EventBits_t bits_to_wait_for,
    BaseType_t clear_on_exit,
    BaseType_t wait_for_all_bits,
    TickType_t ticks_to_wait
);

EventBits_t xEventGroupGetBits(EventGroupHandle_t event_group);
