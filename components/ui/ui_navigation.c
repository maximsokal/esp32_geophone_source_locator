#include "ui_navigation.h"

#include <stddef.h>

#include "ui_screen_router.h"
#include "ui_state.h"


static const ui_screen_t HOME_TARGETS[
    UI_MENU_ITEM_COUNT
] = {
    UI_SCREEN_OBJECTS,
    UI_SCREEN_MEASUREMENTS,
    UI_SCREEN_STORAGE,
    UI_SCREEN_WIFI,
    UI_SCREEN_SYSTEM
};


esp_err_t ui_navigation_open_screen(
    ui_screen_t screen
)
{
    if (
        screen < UI_SCREEN_HOME ||
        screen >= UI_SCREEN_COUNT
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    ui_state_set_screen(
        screen
    );

    return
        ui_screen_open(
            screen
        );
}


esp_err_t ui_navigation_open_selected_home(void)
{
    size_t selected =
        ui_state_home_selection();

    if (
        selected >=
        UI_MENU_ITEM_COUNT
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    return
        ui_navigation_open_screen(
            HOME_TARGETS[
                selected
            ]
        );
}


esp_err_t ui_navigation_return_home(void)
{
    ui_state_set_screen(
        UI_SCREEN_HOME
    );

    return
        ui_screen_draw(
            UI_SCREEN_HOME
        );
}
