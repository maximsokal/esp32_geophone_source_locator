#include "wifi_manager_credentials.h"

#include <string.h>

#include "esp_wifi.h"


#define WIFI_MANAGER_SSID_MAX_LEN        32
#define WIFI_MANAGER_PASSWORD_MAX_LEN    63


static esp_err_t wifi_manager_credentials_get_config(
    wifi_config_t *config
)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(
        config,
        0,
        sizeof(*config)
    );

    return
        esp_wifi_get_config(
            WIFI_IF_STA,
            config
        );
}


esp_err_t wifi_manager_credentials_has_saved(
    bool *has_credentials
)
{
    if (has_credentials == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t config;

    esp_err_t err =
        wifi_manager_credentials_get_config(
            &config
        );

    if (err != ESP_OK) {
        return err;
    }

    *has_credentials =
        config.sta.ssid[0] != '\0';

    return ESP_OK;
}


esp_err_t wifi_manager_credentials_save(
    const char *ssid,
    const char *password
)
{
    if (
        ssid == NULL ||
        password == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t ssid_len =
        strlen(ssid);

    const size_t password_len =
        strlen(password);

    if (
        ssid_len == 0 ||
        ssid_len > WIFI_MANAGER_SSID_MAX_LEN ||
        password_len > WIFI_MANAGER_PASSWORD_MAX_LEN
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t config = {0};

    memcpy(
        config.sta.ssid,
        ssid,
        ssid_len
    );

    memcpy(
        config.sta.password,
        password,
        password_len
    );

    return
        esp_wifi_set_config(
            WIFI_IF_STA,
            &config
        );
}


esp_err_t wifi_manager_credentials_clear(void)
{
    wifi_config_t config = {0};

    return
        esp_wifi_set_config(
            WIFI_IF_STA,
            &config
        );
}


esp_err_t wifi_manager_credentials_get_ssid(
    char *ssid,
    size_t ssid_size
)
{
    if (
        ssid == NULL ||
        ssid_size == 0
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t config;

    esp_err_t err =
        wifi_manager_credentials_get_config(
            &config
        );

    if (err != ESP_OK) {
        return err;
    }

    strlcpy(
        ssid,
        (const char *)config.sta.ssid,
        ssid_size
    );

    return ESP_OK;
}
