#include "ui_internal.h"


void ui_render_home_locked(void)
{
    ui_context_t *context =
        ui_context();

    lv_obj_t *screen =
        ui_create_screen_base_locked(
            "MAIN",
            "UP/DOWN NAV   OK/RIGHT OPEN"
        );

    if (screen == NULL) {
        return;
    }

    static const char *items[5] = {
        "OBJECTS",
        "MEASUREMENTS / AD7606",
        "STORAGE / SD CARD",
        "WI-FI / WEB",
        "SYSTEM",
    };

    for (size_t i = 0; i < 5; ++i) {
        context->home_rows[i] =
            ui_create_row_locked(
                screen,
                40 + (int)(i * 34U),
                items[i],
                i == context->selected
            );
    }
}
