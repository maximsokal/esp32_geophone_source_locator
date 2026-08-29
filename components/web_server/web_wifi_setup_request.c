#include "web_server_internal.h"


esp_err_t web_wifi_setup_request_read_body(
    httpd_req_t *request,
    char *body,
    size_t body_size
)
{
    if (
        request == NULL ||
        body == NULL ||
        request->content_len <= 0 ||
        request->content_len >= (int)body_size
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t received = 0;

    while (received < (size_t)request->content_len) {
        const int result =
            httpd_req_recv(
                request,
                body + received,
                request->content_len - received
            );

        if (result <= 0) {
            return ESP_FAIL;
        }

        received += (size_t)result;
    }

    body[received] = '\0';

    return ESP_OK;
}
