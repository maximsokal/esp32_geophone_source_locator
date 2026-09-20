#include "wifi_manager_sta.h"

#include "wifi_manager_setup_events.h"
#include "wifi_manager_sta_events.h"

#include "esp_log.h"
#include "esp_wifi.h"


static const char *TAG =
    "WIFI_STA";


esp_err_t wifi_manager_sta_init(void)
{
    return
        wifi_manager_sta_events_init();
}


esp_err_t wifi_manager_sta_start(void)
{
    /* Start every STA target from a retired/no-IP event baseline. */
    wifi_manager_sta_events_reset();

    esp_err_t err =
        esp_wifi_set_mode(
            WIFI_MODE_STA
        );

    if (err != ESP_OK) {
        return err;
    }

    /* WIFI_EVENT_STA_START may be dispatched immediately after start. Open
     * admission only after the target mode is configured successfully. */
    wifi_manager_sta_events_set_connect_enabled(
        true
    );

    ESP_LOGI(
        TAG,
        "STA PHY bring-up begin"
    );

    err =
        esp_wifi_start();

    if (err != ESP_OK) {
        wifi_manager_sta_events_reset();
        return err;
    }

    ESP_LOGI(
        TAG,
        "STA PHY bring-up complete"
    );

    /*
     * Keep the STA at the ESP-IDF minimum modem-power-save policy. The
     * application is powered through a shared rail with the TFT, SD, INA3221
     * and external ADC; forcing the radio permanently awake here needlessly turns every
     * connected session into a continuous radio load and makes a marginal
     * rail collapse more likely after startup. HTTPS correctness does not
     * require the radio to stay awake between beacon intervals.
     */
    err =
        esp_wifi_set_ps(
            WIFI_PS_MIN_MODEM
        );

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Failed to keep STA modem power save enabled: %s",
            esp_err_to_name(err)
        );

        /* Wi-Fi itself is already running; do not turn this into boot failure. */
        return ESP_OK;
    }

    ESP_LOGI(
        TAG,
        "STA modem power save enabled (minimum modem policy)"
    );

    return ESP_OK;
}


void wifi_manager_sta_reset_state(void)
{
    /* This is the single event-lifetime retire boundary used before every
     * normal/abort/uncertain esp_wifi_stop() path in wifi_manager_mode.c. */
    wifi_manager_sta_events_reset();
    wifi_manager_setup_events_reset();
}
