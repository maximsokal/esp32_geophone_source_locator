#include "web_server_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>


static esp_err_t web_json_writer_flush(
    web_json_writer_t *writer
)
{
    if (writer->status != ESP_OK) {
        return writer->status;
    }

    if (writer->length == 0) {
        return ESP_OK;
    }

    writer->status =
        httpd_resp_send_chunk(
            writer->request,
            writer->buffer,
            writer->length
        );

    writer->length = 0;

    return writer->status;
}


void web_json_writer_init(
    web_json_writer_t *writer,
    httpd_req_t *request
)
{
    writer->request = request;
    writer->length = 0;
    writer->status = ESP_OK;
}


esp_err_t web_json_writer_write(
    web_json_writer_t *writer,
    const char *text
)
{
    if (
        writer == NULL ||
        text == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    if (writer->status != ESP_OK) {
        return writer->status;
    }

    size_t text_length = strlen(text);

    if (text_length > sizeof(writer->buffer)) {
        esp_err_t err = web_json_writer_flush(writer);

        if (err != ESP_OK) {
            return err;
        }

        writer->status =
            httpd_resp_send_chunk(
                writer->request,
                text,
                text_length
            );

        return writer->status;
    }

    if (
        writer->length + text_length >
        sizeof(writer->buffer)
    ) {
        esp_err_t err = web_json_writer_flush(writer);

        if (err != ESP_OK) {
            return err;
        }
    }

    memcpy(
        writer->buffer + writer->length,
        text,
        text_length
    );

    writer->length += text_length;

    return ESP_OK;
}


esp_err_t web_json_writer_printf(
    web_json_writer_t *writer,
    const char *format,
    ...
)
{
    char text[128];

    va_list args;

    va_start(args, format);

    int length =
        vsnprintf(
            text,
            sizeof(text),
            format,
            args
        );

    va_end(args);

    if (
        length < 0 ||
        length >= (int)sizeof(text)
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    return web_json_writer_write(writer, text);
}


esp_err_t web_json_writer_finish(
    web_json_writer_t *writer
)
{
    esp_err_t err = web_json_writer_flush(writer);

    if (err != ESP_OK) {
        return err;
    }

    return httpd_resp_send_chunk(
        writer->request,
        NULL,
        0
    );
}
