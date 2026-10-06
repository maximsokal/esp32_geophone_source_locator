#include <assert.h>

#include "ui_hardware_model.h"
#include "ui_theme.h"


static void expect_state(
    hw_module_state_t state,
    ui_i18n_key_t label,
    uint32_t color
)
{
    const ui_hardware_state_style_t style = ui_hardware_state_style(state);
    assert(style.label_key == label);
    assert(style.color == color);
}


static void test_state_labels_and_colors(void)
{
    expect_state(HW_STATE_UNKNOWN, UI_I18N_KEY_TFT_HW_STATE_UNKNOWN, UI_COLOR_MUTED);
    expect_state(HW_STATE_PRESENT, UI_I18N_KEY_TFT_HW_STATE_PRESENT, UI_COLOR_GOOD);
    expect_state(HW_STATE_ABSENT, UI_I18N_KEY_TFT_HW_STATE_ABSENT, UI_COLOR_WARNING);
    expect_state(HW_STATE_FAULT, UI_I18N_KEY_TFT_HW_STATE_FAULT, UI_COLOR_ERROR);
    expect_state(HW_STATE_DEGRADED, UI_I18N_KEY_TFT_HW_STATE_DEGRADED, UI_COLOR_WARNING);
    expect_state((hw_module_state_t)99, UI_I18N_KEY_TFT_HW_STATE_UNKNOWN, UI_COLOR_MUTED);
}


static void test_pod_state_labels_and_colors(void)
{
    static const struct {
        ui_hardware_pod_state_t state;
        ui_i18n_key_t label;
        uint32_t color;
    } cases[] = {
        {UI_HARDWARE_POD_STATE_READY, UI_I18N_KEY_TFT_HW_STATE_PRESENT, UI_COLOR_GOOD},
        {UI_HARDWARE_POD_STATE_DISCONNECTED, UI_I18N_KEY_TFT_HW_STATE_ABSENT, UI_COLOR_WARNING},
        {UI_HARDWARE_POD_STATE_WARNING, UI_I18N_KEY_TFT_HW_STATE_DEGRADED, UI_COLOR_WARNING},
        {UI_HARDWARE_POD_STATE_ERROR, UI_I18N_KEY_TFT_HW_STATE_FAULT, UI_COLOR_ERROR},
    };

    for (size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        const ui_hardware_state_style_t style =
            ui_hardware_pod_state_style(cases[index].state);
        assert(style.label_key == cases[index].label);
        assert(style.color == cases[index].color);
    }
    const ui_hardware_state_style_t invalid =
        ui_hardware_pod_state_style((ui_hardware_pod_state_t)99);
    assert(invalid.label_key == UI_I18N_KEY_TFT_HW_STATE_UNKNOWN);
    assert(invalid.color == UI_COLOR_MUTED);
}


static void test_module_and_requirement_labels(void)
{
    static const ui_i18n_key_t module_labels[HW_MODULE_COUNT] = {
        UI_I18N_KEY_TFT_HW_MODULE_SD_CARD,
        UI_I18N_KEY_TFT_HW_MODULE_AD7606,
        UI_I18N_KEY_TFT_HW_MODULE_SENSOR_MUX,
        UI_I18N_KEY_TFT_HW_MODULE_MAIN_XIAO,
        UI_I18N_KEY_TFT_HW_MODULE_BAROMETER,
        UI_I18N_KEY_TFT_HW_MODULE_POWER_MONITOR,
    };

    for (int id = 0; id < HW_MODULE_COUNT; ++id) {
        assert(ui_hardware_module_label((hw_module_id_t)id) == module_labels[id]);
    }
    assert(ui_hardware_module_label(HW_MODULE_COUNT) == UI_I18N_KEY_COUNT);

    assert(ui_hardware_requirement_label(HW_REQ_REQUIRED) == UI_I18N_KEY_TFT_HW_REQUIRED);
    assert(ui_hardware_requirement_label(HW_REQ_OPTIONAL) == UI_I18N_KEY_TFT_HW_OPTIONAL);
    assert(ui_hardware_requirement_label((hw_module_requirement_t)99) == UI_I18N_KEY_COUNT);

    static const ui_i18n_key_t pod_titles[] = {
        UI_I18N_KEY_MEASUREMENT_NODE_G1,
        UI_I18N_KEY_MEASUREMENT_NODE_G2,
        UI_I18N_KEY_MEASUREMENT_NODE_G3,
        UI_I18N_KEY_MEASUREMENT_NODE_G4,
    };
    for (size_t index = 0U; index < sizeof(pod_titles) / sizeof(pod_titles[0]); ++index) {
        assert(ui_hardware_pod_slot_title(index) == pod_titles[index]);
    }
    assert(ui_hardware_pod_slot_title(sizeof(pod_titles) / sizeof(pod_titles[0])) == UI_I18N_KEY_COUNT);
}


int main(void)
{
    test_state_labels_and_colors();
    test_pod_state_labels_and_colors();
    test_module_and_requirement_labels();
    return 0;
}
