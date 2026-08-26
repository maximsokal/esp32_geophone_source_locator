#include "wifi_manager_runtime.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"


typedef struct
{
    bool connected;
    bool known;
    uint8_t mac[6];

} wifi_setup_client_state_t;


static portMUX_TYPE s_client_lock =
    portMUX_INITIALIZER_UNLOCKED;

static wifi_setup_client_state_t s_client;


void wifi_manager_runtime_set_setup_client(
    const uint8_t mac[6],
    bool known
)
{
    if (mac == NULL) {
        return;
    }

    portENTER_CRITICAL(
        &s_client_lock
    );

    memcpy(
        s_client.mac,
        mac,
        sizeof(s_client.mac)
    );

    s_client.connected =
        true;

    s_client.known =
        known;

    portEXIT_CRITICAL(
        &s_client_lock
    );
}


void wifi_manager_runtime_clear_setup_client(void)
{
    portENTER_CRITICAL(
        &s_client_lock
    );

    memset(
        &s_client,
        0,
        sizeof(s_client)
    );

    portEXIT_CRITICAL(
        &s_client_lock
    );
}


bool wifi_manager_runtime_setup_client_connected(void)
{
    bool connected;

    portENTER_CRITICAL(
        &s_client_lock
    );

    connected =
        s_client.connected;

    portEXIT_CRITICAL(
        &s_client_lock
    );

    return connected;
}


bool wifi_manager_runtime_setup_client_known(void)
{
    bool known;

    portENTER_CRITICAL(
        &s_client_lock
    );

    known =
        s_client.known;

    portEXIT_CRITICAL(
        &s_client_lock
    );

    return known;
}


esp_err_t wifi_manager_runtime_setup_client_mac(
    uint8_t mac[6]
)
{
    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(
        &s_client_lock
    );

    if (!s_client.connected) {
        portEXIT_CRITICAL(
            &s_client_lock
        );

        return ESP_ERR_NOT_FOUND;
    }

    memcpy(
        mac,
        s_client.mac,
        6
    );

    portEXIT_CRITICAL(
        &s_client_lock
    );

    return ESP_OK;
}
