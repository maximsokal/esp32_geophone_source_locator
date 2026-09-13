#include "wifi_manager.h"

#include "wifi_manager_credentials.h"
#include "wifi_manager_internal.h"
#include "wifi_manager_platform.h"
#include "wifi_manager_setup_ap.h"
#include "wifi_manager_sta.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"


static const char *TAG =
    "WIFI_MANAGER";


static wifi_manager_internal_state_t s_state = { 0 };

static portMUX_TYPE s_state_lock =
    portMUX_INITIALIZER_UNLOCKED;


void wifi_manager_internal_get_state(
    wifi_manager_internal_state_t *state
)
{
    if (state == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_state_lock);
    *state = s_state;
    portEXIT_CRITICAL(&s_state_lock);
}


bool wifi_manager_internal_initialized(void)
{
    wifi_manager_internal_state_t state;
    wifi_manager_internal_get_state(&state);
    return state.initialized;
}


esp_err_t wifi_manager_internal_begin_transition(void)
{
    esp_err_t result = ESP_OK;

    portENTER_CRITICAL(&s_state_lock);
    if (s_state.transition_active) {
        result = ESP_ERR_INVALID_STATE;
    }
    else {
        s_state.transition_active = true;
    }
    portEXIT_CRITICAL(&s_state_lock);

    return result;
}


void wifi_manager_internal_end_transition(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.transition_active = false;
    portEXIT_CRITICAL(&s_state_lock);
}


void wifi_manager_internal_set_initialized(
    bool initialized
)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.initialized = initialized;
    portEXIT_CRITICAL(&s_state_lock);
}


void wifi_manager_internal_commit_state(
    bool running,
    bool setup_mode,
    bool radio_state_uncertain
)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.running = running;
    s_state.setup_mode = running && setup_mode;
    s_state.radio_state_uncertain = radio_state_uncertain;
    portEXIT_CRITICAL(&s_state_lock);
}


bool wifi_manager_internal_setup_mode(void)
{
    wifi_manager_internal_state_t state;
    wifi_manager_internal_get_state(&state);
    return state.running && state.setup_mode;
}


void wifi_manager_internal_set_radio_state_uncertain(
    bool uncertain
)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.radio_state_uncertain = uncertain;
    portEXIT_CRITICAL(&s_state_lock);
}


bool wifi_manager_internal_radio_state_uncertain(void)
{
    wifi_manager_internal_state_t state;
    wifi_manager_internal_get_state(&state);
    return state.radio_state_uncertain;
}


esp_err_t wifi_manager_internal_start_setup_mode(void)
{
    const esp_err_t err =
        wifi_manager_setup_ap_start(
            wifi_manager_platform_ap_netif()
        );

    if (err == ESP_OK) {
        ESP_LOGI(
            TAG,
            "Setup mode: local captive portal"
        );
    }

    return err;
}


esp_err_t wifi_manager_init(void)
{
    if (wifi_manager_internal_initialized()) {
        return ESP_OK;
    }

    esp_err_t err =
        wifi_manager_internal_begin_transition();
    if (err != ESP_OK) {
        return err;
    }

    if (wifi_manager_internal_initialized()) {
        wifi_manager_internal_end_transition();
        return ESP_OK;
    }

    err =
        wifi_manager_platform_init();
    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Wi-Fi platform initialization failed: %s",
            esp_err_to_name(err)
        );
        wifi_manager_internal_end_transition();
        return err;
    }

    err =
        wifi_manager_sta_init();
    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Wi-Fi STA events initialization failed: %s",
            esp_err_to_name(err)
        );
        wifi_manager_internal_end_transition();
        return err;
    }

    wifi_manager_internal_set_initialized(true);
    wifi_manager_internal_end_transition();

    ESP_LOGI(
        TAG,
        "Initialized"
    );

    return ESP_OK;
}


esp_err_t wifi_manager_start(void)
{
    wifi_manager_internal_state_t state;
    wifi_manager_internal_get_state(&state);

    if (!state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (state.radio_state_uncertain) {
        return ESP_ERR_INVALID_STATE;
    }

    if (state.running) {
        return ESP_OK;
    }

    esp_err_t err =
        wifi_manager_internal_begin_transition();
    if (err != ESP_OK) {
        return err;
    }

    wifi_manager_internal_get_state(&state);
    if (!state.initialized || state.radio_state_uncertain) {
        wifi_manager_internal_end_transition();
        return ESP_ERR_INVALID_STATE;
    }
    if (state.running) {
        wifi_manager_internal_end_transition();
        return ESP_OK;
    }

    bool has_credentials = false;

    err =
        wifi_manager_credentials_has_saved(
            &has_credentials
        );
    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to inspect saved Wi-Fi credentials: %s",
            esp_err_to_name(err)
        );
        wifi_manager_internal_end_transition();
        return err;
    }

    if (has_credentials) {
        err =
            wifi_manager_sta_start();

        if (err != ESP_OK) {
            wifi_manager_internal_end_transition();
            return err;
        }

        ESP_LOGI(
            TAG,
            "Normal mode: Wi-Fi STA"
        );
    }
    else {
        err =
            wifi_manager_internal_start_setup_mode();

        if (err != ESP_OK) {
            wifi_manager_internal_end_transition();
            return err;
        }
    }

    wifi_manager_internal_commit_state(
        true,
        !has_credentials,
        false
    );
    wifi_manager_internal_end_transition();

    return ESP_OK;
}


bool wifi_manager_is_running(void)
{
    wifi_manager_internal_state_t state;
    wifi_manager_internal_get_state(&state);
    return state.running;
}


bool wifi_manager_is_radio_state_uncertain(void)
{
    return wifi_manager_internal_radio_state_uncertain();
}


bool wifi_manager_is_setup_mode(void)
{
    wifi_manager_internal_state_t state;
    wifi_manager_internal_get_state(&state);
    return state.running && state.setup_mode;
}
