#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "web_server_internal.h"

static void test_url_decode(void)
{
    char output[64];
    assert(web_wifi_setup_url_decode(
        "Home+Net%2D5G%2fA%20B",
        output,
        sizeof(output)
    ) == ESP_OK);
    assert(strcmp(output, "Home Net-5G/A B") == 0);

    assert(web_wifi_setup_url_decode("%", output, sizeof(output)) == ESP_ERR_INVALID_ARG);
    assert(web_wifi_setup_url_decode("%A", output, sizeof(output)) == ESP_ERR_INVALID_ARG);
    assert(web_wifi_setup_url_decode("%GG", output, sizeof(output)) == ESP_ERR_INVALID_ARG);
    assert(web_wifi_setup_url_decode("abc", output, 3U) == ESP_ERR_INVALID_SIZE);
    assert(web_wifi_setup_url_decode(NULL, output, sizeof(output)) == ESP_ERR_INVALID_ARG);
    assert(web_wifi_setup_url_decode("abc", NULL, sizeof(output)) == ESP_ERR_INVALID_ARG);
    assert(web_wifi_setup_url_decode("abc", output, 0U) == ESP_ERR_INVALID_ARG);
}

static void test_form_parse(void)
{
    char body[] = "ssid=Home+Net&password=p%40ss%26word";
    char ssid[33];
    char password[64];
    assert(web_wifi_setup_form_parse(
        body,
        ssid,
        sizeof(ssid),
        password,
        sizeof(password)
    ) == ESP_OK);
    assert(strcmp(ssid, "Home Net") == 0);
    assert(strcmp(password, "p@ss&word") == 0);

    char no_password[] = "ssid=only-ssid";
    assert(web_wifi_setup_form_parse(
        no_password,
        ssid,
        sizeof(ssid),
        password,
        sizeof(password)
    ) == ESP_OK);
    assert(strcmp(ssid, "only-ssid") == 0);
    assert(password[0] == '\0');

    char no_ssid[] = "password=secret";
    assert(web_wifi_setup_form_parse(
        no_ssid,
        ssid,
        sizeof(ssid),
        password,
        sizeof(password)
    ) == ESP_ERR_INVALID_ARG);

    char prefix_collision[] = "xssid=wrong&password=secret";
    assert(web_wifi_setup_form_parse(
        prefix_collision,
        ssid,
        sizeof(ssid),
        password,
        sizeof(password)
    ) == ESP_ERR_INVALID_ARG);

    /* The limit is WEB_WIFI_SETUP_BODY_MAX - 1 characters: the largest legal SSID
     * and password, fully percent-encoded, is 300 characters. */
    char too_long_body[WEB_WIFI_SETUP_BODY_MAX + 1U];
    memset(too_long_body, 'x', sizeof(too_long_body) - 1U);
    too_long_body[sizeof(too_long_body) - 1U] = '\0';
    assert(web_wifi_setup_form_parse(
        too_long_body,
        ssid,
        sizeof(ssid),
        password,
        sizeof(password)
    ) == ESP_ERR_INVALID_SIZE);
    too_long_body[WEB_WIFI_SETUP_BODY_MAX - 1U] = '\0';
    assert(web_wifi_setup_form_parse(
        too_long_body,
        ssid,
        sizeof(ssid),
        password,
        sizeof(password)
    ) == ESP_ERR_INVALID_ARG); /* within the limit, but it has no ssid field */

    char small_output[] = "ssid=abcdef";
    assert(web_wifi_setup_form_parse(
        small_output,
        ssid,
        4U,
        password,
        sizeof(password)
    ) == ESP_ERR_INVALID_SIZE);
    assert(web_wifi_setup_form_parse(
        NULL,
        ssid,
        sizeof(ssid),
        password,
        sizeof(password)
    ) == ESP_ERR_INVALID_ARG);
}

int httpd_req_recv(
    httpd_req_t *request,
    char *buffer,
    size_t length
)
{
    assert(request != NULL);
    assert(buffer != NULL);
    if (request->recv_override != 0) {
        return request->recv_override;
    }
    assert(request->body != NULL);
    const size_t remaining = (size_t)request->content_len - request->offset;
    const size_t received = remaining < request->chunk_size
        ? remaining
        : request->chunk_size;
    assert(received > 0U);
    assert(received <= length);
    memcpy(buffer, request->body + request->offset, received);
    request->offset += received;
    return (int)received;
}

esp_err_t web_server_route_tracking_register(const char *uri, int method)
{
    (void)uri;
    (void)method;
    return ESP_OK;
}

esp_err_t httpd_register_uri_handler(
    httpd_handle_t server,
    const httpd_uri_t *uri
)
{
    (void)server;
    (void)uri;
    return ESP_OK;
}

static void test_request_body_read(void)
{
    char body[32];
    httpd_req_t request = {
        .content_len = 11,
        .body = "ssid=hello",
        .chunk_size = 2U,
    };
    assert(web_wifi_setup_request_read_body(&request, body, sizeof(body)) == ESP_OK);
    assert(strcmp(body, "ssid=hello") == 0);
    assert(request.offset == 11U);

    request.offset = 0U;
    request.recv_override = 0;
    assert(web_wifi_setup_request_read_body(&request, body, 11U) == ESP_ERR_INVALID_SIZE);
    request.content_len = 0;
    assert(web_wifi_setup_request_read_body(&request, body, sizeof(body)) == ESP_ERR_INVALID_SIZE);
    request.content_len = -1;
    assert(web_wifi_setup_request_read_body(&request, body, sizeof(body)) == ESP_ERR_INVALID_SIZE);

    request.content_len = 11;
    request.offset = 0U;
    request.recv_override = -1;
    assert(web_wifi_setup_request_read_body(&request, body, sizeof(body)) == ESP_FAIL);
    request.recv_override = 0;
    assert(web_wifi_setup_request_read_body(NULL, body, sizeof(body)) == ESP_ERR_INVALID_SIZE);
    assert(web_wifi_setup_request_read_body(&request, NULL, sizeof(body)) == ESP_ERR_INVALID_SIZE);
}

int main(void)
{
    test_url_decode();
    test_form_parse();
    test_request_body_read();
    puts("web Wi-Fi setup input tests passed");
    return 0;
}
