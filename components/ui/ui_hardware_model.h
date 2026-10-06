#pragma once

#include <stddef.h>
#include <stdint.h>

#include "hw_inventory.h"
#include "ui_i18n_catalog.gen.h"

typedef struct {
    ui_i18n_key_t label_key;
    uint32_t color;
} ui_hardware_state_style_t;

typedef enum {
    UI_HARDWARE_POD_STATE_UNKNOWN = 0,
    UI_HARDWARE_POD_STATE_DISCONNECTED,
    UI_HARDWARE_POD_STATE_READY,
    UI_HARDWARE_POD_STATE_WARNING,
    UI_HARDWARE_POD_STATE_ERROR,
} ui_hardware_pod_state_t;

ui_hardware_state_style_t ui_hardware_state_style(hw_module_state_t state);
ui_hardware_state_style_t ui_hardware_pod_state_style(ui_hardware_pod_state_t state);
ui_i18n_key_t ui_hardware_module_label(hw_module_id_t id);
ui_i18n_key_t ui_hardware_requirement_label(hw_module_requirement_t requirement);
ui_i18n_key_t ui_hardware_pod_slot_title(size_t slot_index);
