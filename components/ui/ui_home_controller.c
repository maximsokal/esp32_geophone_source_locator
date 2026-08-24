#include "ui_internal.h"

#include "ui_navigation.h"
#include "ui_state.h"


static esp_err_t ui_home_move_selection(
    bool down
)
{
    size_t previous =
        ui_state_home_selection();

    size_t selected =
        previous;

    if (down) {
        selected++;

        if (
            selected >=
            UI_MENU_ITEM_COUNT
        ) {
            selected =
                0;
        }
    }
    else if (selected == 0) {
        selected =
            UI_MENU_ITEM_COUNT - 1;
    }
    else {
        selected--;
    }

    ui_state_set_home_selection(
        selected
    );

    return
        ui_home_update_selection(
            previous,
            selected
        );
}


esp_err_t ui_home_controller_open(void)
{
    return
        ui_home_controller_draw();
}


esp_err_t ui_home_controller_draw(void)
{
    return
        ui_home_draw(
            ui_state_home_selection()
        );
}


esp_err_t ui_home_controller_handle_key(
    keypad_key_t key
)
{
    switch (key) {
        case KEYPAD_KEY_UP:
            return
                ui_home_move_selection(
                    false
                );

        case KEYPAD_KEY_DOWN:
            return
                ui_home_move_selection(
                    true
                );

        case KEYPAD_KEY_OK:
        case KEYPAD_KEY_RIGHT:
            return
                ui_navigation_open_selected_home();

        case KEYPAD_KEY_LEFT:
        case KEYPAD_KEY_NONE:
        default:
            return ESP_OK;
    }
}
