#include "ui_hardware_model.h"

#include "ui_theme.h"


ui_hardware_state_style_t ui_hardware_state_style(hw_module_state_t state)
{
    switch (state) {
        case HW_STATE_PRESENT:
            return (ui_hardware_state_style_t){
                UI_I18N_KEY_TFT_HW_STATE_PRESENT,
                UI_COLOR_GOOD,
            };
        case HW_STATE_ABSENT:
            return (ui_hardware_state_style_t){
                UI_I18N_KEY_TFT_HW_STATE_ABSENT,
                UI_COLOR_WARNING,
            };
        case HW_STATE_FAULT:
            return (ui_hardware_state_style_t){
                UI_I18N_KEY_TFT_HW_STATE_FAULT,
                UI_COLOR_ERROR,
            };
        case HW_STATE_DEGRADED:
            return (ui_hardware_state_style_t){
                UI_I18N_KEY_TFT_HW_STATE_DEGRADED,
                UI_COLOR_WARNING,
            };
        case HW_STATE_UNKNOWN:
        default:
            return (ui_hardware_state_style_t){
                UI_I18N_KEY_TFT_HW_STATE_UNKNOWN,
                UI_COLOR_MUTED,
            };
    }
}


ui_hardware_state_style_t ui_hardware_pod_state_style(ui_hardware_pod_state_t state)
{
    switch (state) {
        case UI_HARDWARE_POD_STATE_READY:
            return (ui_hardware_state_style_t){
                UI_I18N_KEY_TFT_HW_STATE_PRESENT,
                UI_COLOR_GOOD,
            };
        case UI_HARDWARE_POD_STATE_DISCONNECTED:
            return (ui_hardware_state_style_t){
                UI_I18N_KEY_TFT_HW_STATE_ABSENT,
                UI_COLOR_WARNING,
            };
        case UI_HARDWARE_POD_STATE_WARNING:
            return (ui_hardware_state_style_t){
                UI_I18N_KEY_TFT_HW_STATE_DEGRADED,
                UI_COLOR_WARNING,
            };
        case UI_HARDWARE_POD_STATE_ERROR:
            return (ui_hardware_state_style_t){
                UI_I18N_KEY_TFT_HW_STATE_FAULT,
                UI_COLOR_ERROR,
            };
        case UI_HARDWARE_POD_STATE_UNKNOWN:
        default:
            return (ui_hardware_state_style_t){
                UI_I18N_KEY_TFT_HW_STATE_UNKNOWN,
                UI_COLOR_MUTED,
            };
    }
}


ui_i18n_key_t ui_hardware_module_label(hw_module_id_t id)
{
    switch (id) {
        case HW_MODULE_SD_CARD:
            return UI_I18N_KEY_TFT_HW_MODULE_SD_CARD;
        case HW_MODULE_AD7606:
            return UI_I18N_KEY_TFT_HW_MODULE_AD7606;
        case HW_MODULE_SENSOR_MUX:
            return UI_I18N_KEY_TFT_HW_MODULE_SENSOR_MUX;
        case HW_MODULE_MAIN_XIAO:
            return UI_I18N_KEY_TFT_HW_MODULE_MAIN_XIAO;
        case HW_MODULE_BAROMETER:
            return UI_I18N_KEY_TFT_HW_MODULE_BAROMETER;
        case HW_MODULE_POWER_MONITOR:
            return UI_I18N_KEY_TFT_HW_MODULE_POWER_MONITOR;
        case HW_MODULE_COUNT:
        default:
            return UI_I18N_KEY_COUNT;
    }
}


ui_i18n_key_t ui_hardware_pod_slot_title(size_t slot_index)
{
    static const ui_i18n_key_t titles[] = {
        UI_I18N_KEY_MEASUREMENT_NODE_G1,
        UI_I18N_KEY_MEASUREMENT_NODE_G2,
        UI_I18N_KEY_MEASUREMENT_NODE_G3,
        UI_I18N_KEY_MEASUREMENT_NODE_G4,
    };

    return slot_index < sizeof(titles) / sizeof(titles[0])
        ? titles[slot_index]
        : UI_I18N_KEY_COUNT;
}


ui_i18n_key_t ui_hardware_requirement_label(hw_module_requirement_t requirement)
{
    switch (requirement) {
        case HW_REQ_REQUIRED:
            return UI_I18N_KEY_TFT_HW_REQUIRED;
        case HW_REQ_OPTIONAL:
            return UI_I18N_KEY_TFT_HW_OPTIONAL;
        default:
            return UI_I18N_KEY_COUNT;
    }
}
