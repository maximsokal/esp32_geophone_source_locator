#pragma once

#include <stdbool.h>

typedef enum
{
    HW_MODULE_SD_CARD,

} hw_module_id_t;

typedef enum
{
    HW_STATE_UNKNOWN,
    HW_STATE_PRESENT,

} hw_module_state_t;

typedef struct
{
    hw_module_state_t state;

} hw_module_info_t;

bool hw_inventory_get(hw_module_id_t module, hw_module_info_t *info);
const char *hw_inventory_state_name(hw_module_state_t state);
