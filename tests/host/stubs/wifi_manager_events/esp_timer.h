#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct esp_timer *esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void *arg);

typedef struct {
    esp_timer_cb_t callback;
    void *arg;
    const char *name;
    int dispatch_method;
} esp_timer_create_args_t;

#define ESP_TIMER_TASK 0

esp_err_t esp_timer_create(
    const esp_timer_create_args_t *create_args,
    esp_timer_handle_t *out_handle
);
esp_err_t esp_timer_start_once(
    esp_timer_handle_t timer,
    uint64_t timeout_us
);
esp_err_t esp_timer_stop(esp_timer_handle_t timer);
esp_err_t esp_timer_delete(esp_timer_handle_t timer);

/* The host runtime test harness uses this to trigger timer callbacks. */
void esp_timer_test_fire(void);
