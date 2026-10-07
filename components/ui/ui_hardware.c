#include "ui_internal.h"

#include "device_status.h"
#include "hw_inventory.h"
#include "ui_hardware_model.h"
#include "ui_utf8_format.h"
#include "workflow_service.h"

#include <stdio.h>
#include <string.h>


static workflow_snapshot_t s_workflow_snapshot;
static unsigned s_remote_count;
static ui_hardware_pod_state_t s_pod_states[DEVICE_STATUS_POD_COUNT] = {
    UI_HARDWARE_POD_STATE_UNKNOWN,
    UI_HARDWARE_POD_STATE_UNKNOWN,
    UI_HARDWARE_POD_STATE_UNKNOWN,
    UI_HARDWARE_POD_STATE_UNKNOWN,
};


static ui_hardware_pod_state_t ui_hardware_map_pod_state(pod_state_t state)
{
    switch (state) {
        case POD_STATE_READY:
            return UI_HARDWARE_POD_STATE_READY;
        case POD_STATE_DISCONNECTED:
            return UI_HARDWARE_POD_STATE_DISCONNECTED;
        case POD_STATE_WARNING:
            return UI_HARDWARE_POD_STATE_WARNING;
        case POD_STATE_ERROR:
            return UI_HARDWARE_POD_STATE_ERROR;
        default:
            return UI_HARDWARE_POD_STATE_UNKNOWN;
    }
}


static void ui_hardware_format_remote_summary(
    char *text,
    size_t capacity,
    unsigned remote_count
)
{
    const char *format = ui_tr(UI_I18N_KEY_TFT_HW_REMOTES);
    const char *placeholder = strstr(format, "{count}");
    if (placeholder == NULL) {
        (void)ui_utf8_format(text, capacity, "%s", format);
        return;
    }

    char count[12];
    (void)ui_utf8_format(count, sizeof(count), "%u", remote_count);
    (void)ui_utf8_format(
        text,
        capacity,
        "%.*s%s%s",
        (int)(placeholder - format),
        format,
        count,
        placeholder + sizeof("{count}") - 1U
    );
}


void ui_render_hardware_locked(void)
{
    lv_obj_t *screen = ui_create_screen_base_locked(
        ui_tr(UI_I18N_KEY_TFT_HW_TITLE),
        ui_tr(UI_I18N_KEY_SHARED_FOOTER_BACK)
    );
    if (screen == NULL) {
        return;
    }

    lv_obj_t *card = ui_create_card_locked(screen, 10, 35, 300, 179);
    if (card == NULL) {
        return;
    }

    if (workflow_service_try_get_snapshot(&s_workflow_snapshot) == ESP_OK) {
        s_remote_count = (unsigned)s_workflow_snapshot.assigned_remotes;
    }
    device_status_snapshot_t status;
    if (device_status_try_get_snapshot(&status) == ESP_OK) {
        for (size_t index = 0U; index < DEVICE_STATUS_POD_COUNT; ++index) {
            s_pod_states[index] = ui_hardware_map_pod_state(status.pods[index].state);
        }
    }

    char remote_summary[64];
    ui_hardware_format_remote_summary(
        remote_summary,
        sizeof(remote_summary),
        s_remote_count
    );
    (void)ui_create_label_locked(
        card, 8, 4, 284, remote_summary, UI_COLOR_ACCENT
    );

    hw_module_info_t modules[HW_MODULE_COUNT];
    const size_t count = hw_inventory_snapshot(modules, HW_MODULE_COUNT);
    for (size_t index = 0U; index < count; ++index) {
        const hw_module_info_t *module = &modules[index];
        const ui_hardware_state_style_t state = ui_hardware_state_style(module->state);
        const int y = 24 + (int)(index * 19U);

        (void)ui_create_label_locked(
            card,
            8,
            y,
            118,
            ui_tr(ui_hardware_module_label(module->id)),
            UI_COLOR_TEXT
        );
        (void)ui_create_label_locked(
            card,
            128,
            y,
            104,
            ui_tr(state.label_key),
            state.color
        );
        (void)ui_create_label_locked(
            card,
            232,
            y,
            60,
            ui_tr(ui_hardware_requirement_label(module->requirement)),
            UI_COLOR_MUTED
        );
    }

    for (size_t index = 0U; index < DEVICE_STATUS_POD_COUNT; ++index) {
        const ui_hardware_state_style_t state =
            ui_hardware_pod_state_style(s_pod_states[index]);
        const size_t row = index / 2U;
        const size_t column = index % 2U;
        const int x = column == 0U ? 8 : 150;
        const int y = 139 + (int)(row * 19U);

        (void)ui_create_label_locked(
            card,
            x,
            y,
            32,
            ui_tr(ui_hardware_pod_slot_title(index)),
            UI_COLOR_TEXT
        );
        (void)ui_create_label_locked(
            card,
            x + 35,
            y,
            100,
            ui_tr(state.label_key),
            state.color
        );
    }
}
