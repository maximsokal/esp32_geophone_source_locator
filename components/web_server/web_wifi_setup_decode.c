#include "web_wifi_setup_decode.h"

#include <ctype.h>


static int web_wifi_setup_hex_value(
    char value
)
{
    if (
        value >= '0' &&
        value <= '9'
    ) {
        return
            value - '0';
    }

    value =
        (char)toupper(
            (unsigned char)value
        );

    if (
        value >= 'A' &&
        value <= 'F'
    ) {
        return
            value - 'A' + 10;
    }

    return -1;
}


esp_err_t web_wifi_setup_url_decode(
    const char *source,
    char *destination,
    size_t destination_size
)
{
    if (
        source == NULL ||
        destination == NULL ||
        destination_size == 0
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t output =
        0;

    while (*source != '\0') {
        if (
            output + 1 >=
            destination_size
        ) {
            return ESP_ERR_INVALID_SIZE;
        }

        if (*source == '+') {
            destination[output++] =
                ' ';

            ++source;
            continue;
        }

        if (*source == '%') {
            if (
                source[1] == '\0' ||
                source[2] == '\0'
            ) {
                return ESP_ERR_INVALID_ARG;
            }

            const int high =
                web_wifi_setup_hex_value(
                    source[1]
                );

            const int low =
                web_wifi_setup_hex_value(
                    source[2]
                );

            if (
                high < 0 ||
                low < 0
            ) {
                return ESP_ERR_INVALID_ARG;
            }

            destination[output++] =
                (char)(
                    (high << 4) |
                    low
                );

            source += 3;
            continue;
        }

        destination[output++] =
            *source++;
    }

    destination[output] =
        '\0';

    return ESP_OK;
}
