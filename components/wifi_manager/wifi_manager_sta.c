#include "wifi_manager_sta.h"

#include "wifi_manager_sta_events.h"

#include "esp_wifi.h"


esp_err_t wifi_manager_sta_init(void)
{
    return
        wifi_manager_sta_events_init();
}


esp_err_t wifi_manager_sta_start(void)
{
    wifi_manager_sta_events_set_connect_enabled(
        true
    );

    esp_err_t err =
        esp_wifi_set_mode(
            WIFI_MODE_STA
        );

    if (err != ESP_OK) {
        return err;
    }

    return
        esp_wifi_start();
}


void wifi_manager_sta_reset_state(void)
{
    wifi_manager_sta_events_reset();
}
