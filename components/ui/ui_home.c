#include "ui_internal.h"

#include "hw_inventory.h"


static const char *ui_home_footer_text(
    const ui_context_t *context,
    bool required_missing
)
{
    if (required_missing) {
        return ui_tr(UI_I18N_KEY_TFT_HW_DEGRADED_BANNER);
    }
    if (context->storage_recovery_active) {
        return ui_tr(UI_I18N_KEY_HOME_CHECKING_STORAGE);
    }
    if (context->storage_recovery_failed) {
        return ui_tr(UI_I18N_KEY_HOME_STORAGE_INCOMPLETE);
    }
    return ui_tr(UI_I18N_KEY_HOME_NAVIGATION);
}


void ui_render_home_locked(void)
{
    ui_context_t *context = ui_context();
    const bool required_missing = hw_inventory_required_missing(NULL);
    context->home_required_missing = required_missing;

    lv_obj_t *screen = ui_create_screen_base_locked(
        ui_tr(UI_I18N_KEY_HOME_TITLE),
        ui_home_footer_text(context, required_missing)
    );

    if (screen == NULL) {
        return;
    }

    if (required_missing && context->footer_label != NULL) {
        lv_obj_set_style_text_color(
            context->footer_label,
            lv_color_hex(UI_COLOR_WARNING),
            0
        );
    }

    const size_t count = ui_screen_home_count();

    if (context->selected >= count) {
        context->selected = 0;
    }

    for (size_t i = 0; i < count; ++i) {
        context->home_rows[i] = ui_create_row_locked(
            screen,
            40 + (int)i * UI_MENU_ROW_STEP,
            ui_screen_home_label(i),
            i == context->selected,
            true
        );
    }
}


void ui_home_refresh_locked(void)
{
    ui_context_t *context = ui_context();
    if (context->footer_label == NULL) {
        return;
    }

    const bool required_missing = hw_inventory_required_missing(NULL);
    if (required_missing == context->home_required_missing) {
        return;
    }

    context->home_required_missing = required_missing;
    lv_label_set_text(
        context->footer_label,
        ui_home_footer_text(context, required_missing)
    );
    lv_obj_set_style_text_color(
        context->footer_label,
        lv_color_hex(required_missing ? UI_COLOR_WARNING : UI_COLOR_MUTED),
        0
    );
}
