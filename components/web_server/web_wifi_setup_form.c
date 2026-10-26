#include "web_server_internal.h"

#include <stdbool.h>
#include <string.h>


static char *web_wifi_setup_form_value(
    char *body,
    const char *key
)
{
    const size_t key_length = strlen(key);
    char *field = body;

    while (
        field != NULL &&
        *field != '\0'
    ) {
        char *next = strchr(field, '&');

        if (next != NULL) {
            *next = '\0';
        }

        if (
            strncmp(
                field,
                key,
                key_length
            ) == 0 &&
            field[key_length] == '='
        ) {
            return field + key_length + 1;
        }

        field = next != NULL ? next + 1 : NULL;
    }

    return NULL;
}


static esp_err_t web_wifi_setup_decode_field(
    const char *body,
    const char *key,
    char *destination,
    size_t destination_size,
    bool required
)
{
    /* 32-byte SSID + 63-byte password, every byte percent-encoded, plus the
     * two keys and separators, is 300 bytes: the limit must not be below that. */
    char copy[WEB_WIFI_SETUP_BODY_MAX];

    if (strlen(body) >= sizeof(copy)) {
        return ESP_ERR_INVALID_SIZE;
    }

    strlcpy(copy, body, sizeof(copy));

    char *encoded =
        web_wifi_setup_form_value(
            copy,
            key
        );

    if (encoded == NULL) {
        if (required) {
            return ESP_ERR_INVALID_ARG;
        }

        destination[0] = '\0';
        return ESP_OK;
    }

    return web_wifi_setup_url_decode(
        encoded,
        destination,
        destination_size
    );
}


esp_err_t web_wifi_setup_form_parse(
    char *body,
    char *ssid,
    size_t ssid_size,
    char *password,
    size_t password_size
)
{
    if (
        body == NULL ||
        ssid == NULL ||
        password == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err =
        web_wifi_setup_decode_field(
            body,
            "ssid",
            ssid,
            ssid_size,
            true
        );

    if (err != ESP_OK) {
        return err;
    }

    return web_wifi_setup_decode_field(
        body,
        "password",
        password,
        password_size,
        false
    );
}
