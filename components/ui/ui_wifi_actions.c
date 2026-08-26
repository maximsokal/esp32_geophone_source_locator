#include "ui_wifi_actions.h"

#include "app_config.h"
#include "ui_internal.h"
#include "ui_wifi_state.h"
#include "wifi_manager.h"

#include "esp_system.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


static esp_err_t ui_wifi_start_sta_flow(void)
{
    esp_err_t err =
        wifi_manager_connect_saved();

    if (err != ESP_OK) {
        return err;
    }

    ui_wifi_state_set_page(
        UI_WIFI_PAGE_WAIT_HOTSPOT
    );

    return
        ui_wifi_draw();
}


esp_err_t ui_wifi_action_activate_setup(void)
{
    esp_err_t err =
        wifi_manager_enter_setup_mode();

    if (err != ESP_OK) {
        return err;
    }

    ui_wifi_state_set_page(
        UI_WIFI_PAGE_CONNECT_SETUP
    );

    return
        ui_wifi_draw();
}


esp_err_t ui_wifi_action_open_setup(void)
{
    if (
        !wifi_manager_is_setup_mode()
    ) {
        return
            ui_wifi_action_activate_setup();
    }

    if (
        !wifi_manager_setup_client_connected()
    ) {
        ui_wifi_state_set_page(
            UI_WIFI_PAGE_CONNECT_SETUP
        );

        return
            ui_wifi_draw();
    }

    ui_wifi_state_set_page(
        UI_WIFI_PAGE_OPEN_SETUP
    );

    return
        ui_wifi_draw();
}


esp_err_t ui_wifi_action_use_quick_preset(void)
{
    esp_err_t err =
        wifi_manager_save_sta_credentials(
            APP_WIFI_QUICK_SSID,
            APP_WIFI_QUICK_PASSWORD
        );

    if (err != ESP_OK) {
        return err;
    }

    return
        ui_wifi_start_sta_flow();
}


esp_err_t ui_wifi_action_connect_saved(void)
{
    return
        ui_wifi_start_sta_flow();
}


esp_err_t ui_wifi_action_forget(void)
{
    esp_err_t err =
        wifi_manager_forget_sta_credentials();

    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(
        pdMS_TO_TICKS(
            300
        )
    );

    esp_restart();

    return ESP_OK;
}
