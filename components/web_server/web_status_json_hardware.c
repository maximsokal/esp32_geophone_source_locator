#include "web_status_json_hardware.h"

#include "hw_inventory.h"

bool web_status_json_add_hardware(cJSON *root)
{
    if (root == NULL) return false;

    hw_module_info_t modules[HW_MODULE_COUNT];
    const size_t count = hw_inventory_snapshot(modules, HW_MODULE_COUNT);
    if (count != (size_t)HW_MODULE_COUNT) return false;

    bool required_missing = false;
    for (size_t i = 0U; i < count; ++i) {
        if (modules[i].requirement == HW_REQ_REQUIRED &&
            (modules[i].state == HW_STATE_ABSENT || modules[i].state == HW_STATE_FAULT)) {
            required_missing = true;
        }
    }

    cJSON *hardware = cJSON_AddObjectToObject(root, "hardware");
    cJSON *module_array = hardware != NULL
        ? cJSON_AddArrayToObject(hardware, "modules")
        : NULL;
    if (hardware == NULL || module_array == NULL) return false;

    for (size_t i = 0U; i < count; ++i) {
        const hw_module_info_t *module = &modules[i];
        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL) return false;
        if (!cJSON_AddItemToArray(module_array, entry)) {
            cJSON_Delete(entry);
            return false;
        }

        if (
            cJSON_AddStringToObject(entry, "id", hw_inventory_module_name(module->id)) == NULL ||
            cJSON_AddStringToObject(entry, "state", hw_inventory_state_name(module->state)) == NULL ||
            cJSON_AddBoolToObject(entry, "required", module->requirement == HW_REQ_REQUIRED) == NULL ||
            cJSON_AddStringToObject(entry, "reason", module->reason) == NULL
        ) {
            return false;
        }
    }

    return cJSON_AddBoolToObject(hardware, "required_missing", required_missing) != NULL;
}
