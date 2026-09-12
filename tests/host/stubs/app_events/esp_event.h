#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef void *esp_event_loop_handle_t;
typedef const char *esp_event_base_t;

typedef struct {
    int32_t queue_size;
    const char *task_name;
    int32_t task_priority;
    int32_t task_stack_size;
    int32_t task_core_id;
} esp_event_loop_args_t;

#define ESP_EVENT_DECLARE_BASE(base) extern esp_event_base_t base
#define ESP_EVENT_DEFINE_BASE(base) esp_event_base_t base = #base

esp_err_t esp_event_loop_create(
    const esp_event_loop_args_t *args,
    esp_event_loop_handle_t *event_loop
);

esp_err_t esp_event_post_to(
    esp_event_loop_handle_t event_loop,
    esp_event_base_t event_base,
    int32_t event_id,
    const void *event_data,
    size_t event_data_size,
    uint32_t ticks_to_wait
);
