#include "wifi_manager_profile.h"

#include <string.h>

#include "nvs.h"


#define WIFI_PROFILE_NAMESPACE    "wifi_profile"
#define WIFI_PROFILE_MAC_KEY      "client_mac"


esp_err_t wifi_manager_profile_is_known_client(
    const uint8_t mac[6],
    bool *known
)
{
    if (
        mac == NULL ||
        known == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    *known =
        false;

    nvs_handle_t handle;

    esp_err_t err =
        nvs_open(
            WIFI_PROFILE_NAMESPACE,
            NVS_READONLY,
            &handle
        );

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }

    if (err != ESP_OK) {
        return err;
    }

    uint8_t stored_mac[6];

    size_t length =
        sizeof(stored_mac);

    err =
        nvs_get_blob(
            handle,
            WIFI_PROFILE_MAC_KEY,
            stored_mac,
            &length
        );

    nvs_close(
        handle
    );

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }

    if (
        err != ESP_OK ||
        length != sizeof(stored_mac)
    ) {
        return
            err != ESP_OK
                ? err
                : ESP_ERR_INVALID_SIZE;
    }

    *known =
        memcmp(
            stored_mac,
            mac,
            sizeof(stored_mac)
        ) == 0;

    return ESP_OK;
}


esp_err_t wifi_manager_profile_remember_client(
    const uint8_t mac[6]
)
{
    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;

    esp_err_t err =
        nvs_open(
            WIFI_PROFILE_NAMESPACE,
            NVS_READWRITE,
            &handle
        );

    if (err != ESP_OK) {
        return err;
    }

    err =
        nvs_set_blob(
            handle,
            WIFI_PROFILE_MAC_KEY,
            mac,
            6
        );

    if (err == ESP_OK) {
        err =
            nvs_commit(
                handle
            );
    }

    nvs_close(
        handle
    );

    return err;
}


esp_err_t wifi_manager_profile_clear(void)
{
    nvs_handle_t handle;

    esp_err_t err =
        nvs_open(
            WIFI_PROFILE_NAMESPACE,
            NVS_READWRITE,
            &handle
        );

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }

    if (err != ESP_OK) {
        return err;
    }

    err =
        nvs_erase_all(
            handle
        );

    if (err == ESP_OK) {
        err =
            nvs_commit(
                handle
            );
    }

    nvs_close(
        handle
    );

    return err;
}
