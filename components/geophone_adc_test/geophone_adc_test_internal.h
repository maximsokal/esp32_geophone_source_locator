#pragma once

#include <stdbool.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


extern TaskHandle_t g_geophone_adc_task;
extern esp_timer_handle_t g_geophone_adc_timer;
extern adc_channel_t g_geophone_adc_channel;
extern adc_cali_handle_t g_geophone_adc_cali;
extern volatile bool g_geophone_adc_running;


esp_err_t geophone_adc_prepare_hardware(void);

void geophone_adc_release_hardware(void);

esp_err_t geophone_adc_calibrate_scale(
    float baseline_raw,
    float *uv_per_count
);

void geophone_adc_worker(
    void *argument
);

void geophone_adc_timer_callback(
    void *argument
);
