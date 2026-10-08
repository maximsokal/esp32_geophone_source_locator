#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct pipeline_timer *esp_timer_handle_t;
typedef struct {
    void (*callback)(void *arg);
    void *arg;
    int dispatch_method;
    const char *name;
    bool skip_unhandled_events;
} esp_timer_create_args_t;

#define ESP_TIMER_TASK 0

int64_t esp_timer_get_time(void);
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *timer);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period_us);
esp_err_t esp_timer_stop(esp_timer_handle_t timer);
esp_err_t esp_timer_delete(esp_timer_handle_t timer);
