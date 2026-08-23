#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"


#define WEB_JSON_WRITER_BUFFER_SIZE  768


typedef struct
{
    httpd_req_t *request;

    char buffer[
        WEB_JSON_WRITER_BUFFER_SIZE
    ];

    size_t length;

    esp_err_t status;

} web_json_writer_t;


void web_json_writer_init(
    web_json_writer_t *writer,
    httpd_req_t *request
);


esp_err_t web_json_writer_write(
    web_json_writer_t *writer,
    const char *text
);


esp_err_t web_json_writer_printf(
    web_json_writer_t *writer,
    const char *format,
    ...
);


esp_err_t web_json_writer_finish(
    web_json_writer_t *writer
);