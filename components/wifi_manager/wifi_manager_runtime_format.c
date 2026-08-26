#include "wifi_manager_runtime.h"

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"


static portMUX_TYPE s_ip_lock =
    portMUX_INITIALIZER_UNLOCKED;

static bool s_ip_valid =
    false;

static esp_ip4_addr_t s_ip;


esp_err_t wifi_manager_runtime_setup_client_mac_string(
    char *text,
    size_t text_size
)
{
    if (
        text == NULL ||
        text_size < 18
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t mac[6];

    esp_err_t err =
        wifi_manager_runtime_setup_client_mac(
            mac
        );

    if (err != ESP_OK) {
        return err;
    }

    snprintf(
        text,
        text_size,
        "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0],
        mac[1],
        mac[2],
        mac[3],
        mac[4],
        mac[5]
    );

    return ESP_OK;
}


void wifi_manager_runtime_set_sta_ip(
    const esp_ip4_addr_t *ip
)
{
    if (ip == NULL) {
        return;
    }

    portENTER_CRITICAL(
        &s_ip_lock
    );

    s_ip =
        *ip;

    s_ip_valid =
        true;

    portEXIT_CRITICAL(
        &s_ip_lock
    );
}


void wifi_manager_runtime_clear_sta_ip(void)
{
    portENTER_CRITICAL(
        &s_ip_lock
    );

    s_ip_valid =
        false;

    s_ip.addr =
        0;

    portEXIT_CRITICAL(
        &s_ip_lock
    );
}


esp_err_t wifi_manager_runtime_sta_ip_string(
    char *text,
    size_t text_size
)
{
    if (
        text == NULL ||
        text_size < 16
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_ip4_addr_t ip;

    portENTER_CRITICAL(
        &s_ip_lock
    );

    if (!s_ip_valid) {
        portEXIT_CRITICAL(
            &s_ip_lock
        );

        return ESP_ERR_NOT_FOUND;
    }

    ip =
        s_ip;

    portEXIT_CRITICAL(
        &s_ip_lock
    );

    snprintf(
        text,
        text_size,
        IPSTR,
        IP2STR(&ip)
    );

    return ESP_OK;
}
