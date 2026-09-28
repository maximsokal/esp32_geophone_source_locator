#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef struct test_pcnt_unit *pcnt_unit_handle_t;
typedef struct test_pcnt_channel *pcnt_channel_handle_t;

typedef enum {
    PCNT_CHANNEL_EDGE_ACTION_HOLD = 0,
    PCNT_CHANNEL_EDGE_ACTION_INCREASE
} pcnt_channel_edge_action_t;

typedef enum {
    PCNT_CHANNEL_LEVEL_ACTION_KEEP = 0
} pcnt_channel_level_action_t;

typedef struct {
    int low_limit;
    int high_limit;
    int intr_priority;
    struct { uint32_t accum_count:1; } flags;
} pcnt_unit_config_t;

typedef struct {
    int edge_gpio_num;
    int level_gpio_num;
} pcnt_chan_config_t;

esp_err_t pcnt_new_unit(const pcnt_unit_config_t *config, pcnt_unit_handle_t *ret_unit);
esp_err_t pcnt_del_unit(pcnt_unit_handle_t unit);
esp_err_t pcnt_unit_add_watch_point(pcnt_unit_handle_t unit, int watch_point);
esp_err_t pcnt_new_channel(pcnt_unit_handle_t unit, const pcnt_chan_config_t *config,
    pcnt_channel_handle_t *ret_channel);
esp_err_t pcnt_del_channel(pcnt_channel_handle_t channel);
esp_err_t pcnt_channel_set_edge_action(pcnt_channel_handle_t channel,
    pcnt_channel_edge_action_t pos_act, pcnt_channel_edge_action_t neg_act);
esp_err_t pcnt_channel_set_level_action(pcnt_channel_handle_t channel,
    pcnt_channel_level_action_t high_act, pcnt_channel_level_action_t low_act);
esp_err_t pcnt_unit_clear_count(pcnt_unit_handle_t unit);
esp_err_t pcnt_unit_enable(pcnt_unit_handle_t unit);
esp_err_t pcnt_unit_disable(pcnt_unit_handle_t unit);
esp_err_t pcnt_unit_start(pcnt_unit_handle_t unit);
esp_err_t pcnt_unit_stop(pcnt_unit_handle_t unit);
esp_err_t pcnt_unit_get_count(pcnt_unit_handle_t unit, int *value);
