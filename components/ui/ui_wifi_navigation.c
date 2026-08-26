#include "ui_wifi_navigation.h"

#include "ui_internal.h"
#include "ui_wifi_actions.h"
#include "ui_wifi_state.h"


static esp_err_t ui_wifi_open_page(
    ui_wifi_page_t page
)
{
    ui_wifi_state_set_page(
        page
    );

    return
        ui_wifi_draw();
}


esp_err_t ui_wifi_navigation_move(
    bool down
)
{
    size_t selection =
        ui_wifi_state_selection();

    if (down) {
        selection =
            (
                selection + 1
            ) %
            UI_WIFI_MENU_ITEM_COUNT;
    } else if (selection == 0) {
        selection =
            UI_WIFI_MENU_ITEM_COUNT - 1;
    } else {
        --selection;
    }

    ui_wifi_state_set_selection(
        selection
    );

    return
        ui_wifi_draw();
}


esp_err_t ui_wifi_navigation_select(void)
{
    switch (
        ui_wifi_state_selection()
    ) {
        case 0:
            return
                ui_wifi_open_page(
                    UI_WIFI_PAGE_QUICK_CONFIRM
                );

        case 1:
            return
                ui_wifi_action_activate_setup();

        case 2:
            return
                ui_wifi_action_open_setup();

        case 3:
            return
                ui_wifi_action_connect_saved();

        case 4:
            return
                ui_wifi_open_page(
                    UI_WIFI_PAGE_OPEN_DEVICE
                );

        case 5:
            return
                ui_wifi_open_page(
                    UI_WIFI_PAGE_FORGET_CONFIRM
                );

        default:
            return ESP_ERR_INVALID_STATE;
    }
}
