#include "sd_card_diagnostics_internal.h"

#include <stdio.h>


esp_err_t sd_card_diagnostics_write_file(
    const char *path,
    const char *text
)
{
    FILE *file =
        fopen(
            path,
            "w"
        );

    if (file == NULL) {
        return ESP_FAIL;
    }

    int written =
        fprintf(
            file,
            "%s",
            text
        );

    if (written < 0) {
        fclose(
            file
        );

        return ESP_FAIL;
    }

    if (
        fflush(
            file
        ) != 0
    ) {
        fclose(
            file
        );

        return ESP_FAIL;
    }

    if (
        fclose(
            file
        ) != 0
    ) {
        return ESP_FAIL;
    }

    return ESP_OK;
}


esp_err_t sd_card_diagnostics_read_file(
    const char *path,
    char *buffer,
    size_t buffer_size
)
{
    if (
        buffer == NULL ||
        buffer_size < 2
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file =
        fopen(
            path,
            "r"
        );

    if (file == NULL) {
        return ESP_FAIL;
    }

    size_t bytes_read =
        fread(
            buffer,
            1,
            buffer_size - 1,
            file
        );

    if (
        ferror(
            file
        )
    ) {
        fclose(
            file
        );

        return ESP_FAIL;
    }

    if (
        fclose(
            file
        ) != 0
    ) {
        return ESP_FAIL;
    }

    buffer[bytes_read] =
        '\0';

    return ESP_OK;
}
