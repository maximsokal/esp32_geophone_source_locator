#include "ui_wifi_events.h"

#include "captive_portal.h"
#include "ui_internal.h"
#include "ui_screen.h"
#include "ui_state.h"
#include "ui_wifi_state.h"
#include "wifi_manager.h"

#include "esp_log.h"


static const char *TAG =
    "UI_WIFI_FLOW";


static esp_err_t ui_wifi_flow_redraw(void)
{
    if (
        ui_state_screen() !=
        UI_SCREEN_WIFI
    ) {
        return ESP_OK;
    }

    return
        ui_wifi_draw();
}


static esp_err_t ui_wifi_flow_set_page(
    ui_wifi_page_t page
)
{
    ui_wifi_state_set_page(
        page
    );

    return
        ui_wifi_flow_redraw();
}


static esp_err_t ui_wifi_flow_credentials_saved(void)
{
    esp_err_t err =
        captive_portal_stop();

    if (err != ESP_OK) {
        return err;
    }

    err =
        wifi_manager_connect_saved();

    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(
        TAG,
        "Credentials saved; waiting for phone hotspot"
    );

    return
        ui_wifi_flow_set_page(
            UI_WIFI_PAGE_WAIT_HOTSPOT
        );
}


esp_err_t ui_wifi_events_handle(
    app_event_type_t type
)
{
    const ui_wifi_page_t page =
        ui_wifi_state_page();

    switch (type) {
        case APP_EVENT_WIFI_SETUP_CLIENT_READY:
            if (
                page ==
                UI_WIFI_PAGE_CONNECT_SETUP
            ) {
                ESP_LOGI(
                    TAG,
                    "Setup phone ready; showing URL QR"
                );

                return
                    ui_wifi_flow_set_page(
                        UI_WIFI_PAGE_OPEN_SETUP
                    );
            }

            return ESP_OK;

        case APP_EVENT_WIFI_SETUP_PAGE_OPENED:
            if (
                page ==
                UI_WIFI_PAGE_OPEN_SETUP
            ) {
                ESP_LOGI(
                    TAG,
                    "Setup form opened on phone"
                );

                return
                    ui_wifi_flow_set_page(
                        UI_WIFI_PAGE_ENTER_CREDENTIALS
                    );
            }

            return ESP_OK;

        case APP_EVENT_WIFI_CREDENTIALS_SAVED:
            return
                ui_wifi_flow_credentials_saved();

        case APP_EVENT_WIFI_STA_CONNECTED:
            if (
                page ==
                    UI_WIFI_PAGE_WAIT_HOTSPOT ||
                page ==
                    UI_WIFI_PAGE_OPEN_DEVICE
            ) {
                ESP_LOGI(
                    TAG,
                    "Hotspot connected; showing device URL QR"
                );

                return
                    ui_wifi_flow_set_page(
                        UI_WIFI_PAGE_OPEN_DEVICE
                    );
            }

            return ESP_OK;

        case APP_EVENT_WIFI_STA_DISCONNECTED:
            if (
                page ==
                UI_WIFI_PAGE_OPEN_DEVICE
            ) {
                ESP_LOGI(
                    TAG,
                    "Hotspot lost; returning to wait screen"
                );

                return
                    ui_wifi_flow_set_page(
                        UI_WIFI_PAGE_WAIT_HOTSPOT
                    );
            }

            return ESP_OK;

        case APP_EVENT_KEYPAD:
        default:
            return ESP_OK;
    }
}
