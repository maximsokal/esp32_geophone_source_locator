#include "ui_internal.h"

#include "ui_wifi_actions.h"
#include "ui_wifi_navigation.h"
#include "ui_wifi_state.h"
#include "wifi_manager.h"


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


esp_err_t ui_wifi_controller_open(void)
{
    ui_wifi_state_reset();

    return
        ui_wifi_draw();
}


esp_err_t ui_wifi_controller_draw(void)
{
    return
        ui_wifi_draw();
}


esp_err_t ui_wifi_controller_handle_key(
    keypad_key_t key
)
{
    const ui_wifi_page_t page =
        ui_wifi_state_page();

    if (
        key == KEYPAD_KEY_LEFT
    ) {
        if (
            page ==
            UI_WIFI_PAGE_MENU
        ) {
            return ESP_ERR_NOT_SUPPORTED;
        }

        return
            ui_wifi_open_page(
                UI_WIFI_PAGE_MENU
            );
    }

    if (
        page ==
        UI_WIFI_PAGE_MENU
    ) {
        if (key == KEYPAD_KEY_UP) {
            return
                ui_wifi_navigation_move(
                    false
                );
        }

        if (key == KEYPAD_KEY_DOWN) {
            return
                ui_wifi_navigation_move(
                    true
                );
        }

        if (
            key == KEYPAD_KEY_OK ||
            key == KEYPAD_KEY_RIGHT
        ) {
            return
                ui_wifi_navigation_select();
        }

        return ESP_OK;
    }

    if (
        page ==
        UI_WIFI_PAGE_QUICK_CONFIRM
    ) {
        if (key == KEYPAD_KEY_OK) {
            return
                ui_wifi_action_use_quick_preset();
        }

        if (key == KEYPAD_KEY_RIGHT) {
            return
                ui_wifi_action_activate_setup();
        }

        return ESP_OK;
    }

    if (
        page ==
            UI_WIFI_PAGE_CONNECT_SETUP &&
        (
            key == KEYPAD_KEY_OK ||
            key == KEYPAD_KEY_RIGHT
        )
    ) {
        if (
            wifi_manager_setup_client_connected()
        ) {
            return
                ui_wifi_open_page(
                    UI_WIFI_PAGE_OPEN_SETUP
                );
        }

        return
            ui_wifi_draw();
    }

    if (
        page ==
            UI_WIFI_PAGE_OPEN_SETUP &&
        (
            key == KEYPAD_KEY_OK ||
            key == KEYPAD_KEY_RIGHT
        )
    ) {
        return
            ui_wifi_open_page(
                UI_WIFI_PAGE_ENTER_CREDENTIALS
            );
    }

    if (
        page ==
            UI_WIFI_PAGE_WAIT_HOTSPOT &&
        (
            key == KEYPAD_KEY_OK ||
            key == KEYPAD_KEY_RIGHT
        )
    ) {
        return
            ui_wifi_open_page(
                UI_WIFI_PAGE_OPEN_DEVICE
            );
    }

    if (
        page ==
            UI_WIFI_PAGE_FORGET_CONFIRM &&
        key ==
            KEYPAD_KEY_OK
    ) {
        return
            ui_wifi_action_forget();
    }

    if (
        page ==
            UI_WIFI_PAGE_OPEN_DEVICE &&
        (
            key == KEYPAD_KEY_OK ||
            key == KEYPAD_KEY_RIGHT
        )
    ) {
        return
            ui_wifi_draw();
    }

    return ESP_OK;
}
