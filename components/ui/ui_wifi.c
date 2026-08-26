#include "ui_internal.h"

#include "ui_wifi_state.h"


esp_err_t ui_wifi_draw(void)
{
    switch (
        ui_wifi_state_page()
    ) {
        case UI_WIFI_PAGE_MENU:
            return
                ui_wifi_menu_view_draw();

        case UI_WIFI_PAGE_QUICK_CONFIRM:
            return
                ui_wifi_quick_view_draw();

        case UI_WIFI_PAGE_CONNECT_SETUP:
            return
                ui_wifi_connect_view_draw();

        case UI_WIFI_PAGE_OPEN_SETUP:
            return
                ui_wifi_open_setup_view_draw();

        case UI_WIFI_PAGE_ENTER_CREDENTIALS:
            return
                ui_wifi_credentials_view_draw();

        case UI_WIFI_PAGE_WAIT_HOTSPOT:
            return
                ui_wifi_hotspot_view_draw();

        case UI_WIFI_PAGE_OPEN_DEVICE:
            return
                ui_wifi_device_view_draw();

        case UI_WIFI_PAGE_FORGET_CONFIRM:
            return
                ui_wifi_forget_view_draw();

        default:
            return ESP_ERR_INVALID_STATE;
    }
}
