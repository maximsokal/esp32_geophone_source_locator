#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef int ledc_mode_t;
typedef int ledc_timer_t;
typedef int ledc_channel_t;
typedef int ledc_timer_bit_t;
typedef int ledc_intr_type_t;
typedef int ledc_clk_cfg_t;

#define LEDC_LOW_SPEED_MODE 0
#define LEDC_TIMER_0 0
#define LEDC_CHANNEL_0 0
#define LEDC_INTR_DISABLE 0
#define LEDC_USE_XTAL_CLK 1

typedef struct {
    ledc_mode_t speed_mode;
    ledc_timer_bit_t duty_resolution;
    ledc_timer_t timer_num;
    uint32_t freq_hz;
    ledc_clk_cfg_t clk_cfg;
    bool deconfigure;
} ledc_timer_config_t;

typedef struct {
    int gpio_num;
    ledc_mode_t speed_mode;
    ledc_channel_t channel;
    ledc_intr_type_t intr_type;
    ledc_timer_t timer_sel;
    uint32_t duty;
    int hpoint;
} ledc_channel_config_t;

uint32_t ledc_find_suitable_duty_resolution(uint32_t src_clk_freq, uint32_t timer_freq);
esp_err_t ledc_timer_config(const ledc_timer_config_t *config);
esp_err_t ledc_channel_config(const ledc_channel_config_t *config);
uint32_t ledc_get_freq(ledc_mode_t speed_mode, ledc_timer_t timer_num);
esp_err_t ledc_stop(ledc_mode_t speed_mode, ledc_channel_t channel, uint32_t idle_level);
esp_err_t ledc_timer_pause(ledc_mode_t speed_mode, ledc_timer_t timer_num);
