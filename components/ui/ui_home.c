#include "ui_internal.h"


void ui_render_home_locked(void)
{
    ui_context_t *context = ui_context();

    const char *footer =
        context->storage_recovery_active
            ? ui_tr(UI_I18N_KEY_HOME_CHECKING_STORAGE)
            : context->storage_recovery_failed
                ? ui_tr(UI_I18N_KEY_HOME_STORAGE_INCOMPLETE)
                : ui_tr(UI_I18N_KEY_HOME_NAVIGATION);

    lv_obj_t *screen = ui_create_screen_base_locked(
        ui_tr(UI_I18N_KEY_HOME_TITLE),
        footer
    );

    if (screen == NULL) {
        return;
    }

    const size_t count = ui_screen_home_count();

    if (context->selected >= count) {
        context->selected = 0;
    }

    for (size_t i = 0; i < count; ++i) {
        context->home_rows[i] = ui_create_row_locked(
            screen,
            40 + (int)(i * 34U),
            ui_screen_home_label(i),
            i == context->selected,
            true
        );
    }
}
