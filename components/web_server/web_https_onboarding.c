#include "web_server_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "app_events.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"


static const char *TAG =
    "WEB_HTTPS_ONBOARD";


#define WEB_ONBOARDING_QUERY_SIZE      64U
#define WEB_ONBOARDING_TOKEN_SIZE      16U
#define WEB_ONBOARDING_REPORT_SIZE     512U


static bool s_ca_downloaded =
    false;

static bool s_ca_install_confirmed =
    false;

static uint32_t s_verify_nonce =
    0;


#if WEB_SERVER_ROOT_CA_EMBEDDED
extern const unsigned char root_ca_crt_start[]
    asm("_binary_rootCA_crt_start");
extern const unsigned char root_ca_crt_end[]
    asm("_binary_rootCA_crt_end");
#endif


static const char s_ca_page[] =
    "<!doctype html>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>GeoLocator Root CA</title>"
    "<style>body{font-family:sans-serif;max-width:620px;margin:32px auto;padding:0 18px;line-height:1.45}"
    "a,button{display:inline-block;padding:12px 16px;margin:8px 0;background:#155eef;color:white;text-decoration:none;border:0;border-radius:8px}"
    ".note{padding:12px;background:#f2f4f7;border-radius:8px}</style>"
    "<h2>GeoLocator HTTPS certificate</h2>"
    "<p>Download the public GeoLocator Root CA certificate from this ESP32.</p>"
    "<p><a href='/setup/ca.crt'>Download GeoLocator Root CA</a></p>"
    "<div class=note><b>Important:</b> only the public CA certificate is downloaded. The Root CA private key never leaves the development computer.</div>"
    "<p>Install the downloaded CA certificate in the phone's certificate/security settings. The exact menu wording varies by Android version and vendor.</p>"
    "<p>After Android confirms the CA installation, return to this page and continue.</p>"
    "<p><a href='/setup/ca?installed=1'>I installed the CA - continue</a></p>";


static const char s_ca_confirmed_page[] =
    "<!doctype html>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>GeoLocator Root CA</title>"
    "<style>body{font-family:sans-serif;max-width:620px;margin:32px auto;padding:0 18px;line-height:1.45}</style>"
    "<h2>CA installation confirmed</h2>"
    "<p>Return to GeoLocator and scan the next QR code to configure the phone hotspot.</p>";


static const char s_verify_page[] =
    "<!doctype html>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>GeoLocator HTTPS Verify</title>"
    "<style>body{font-family:sans-serif;max-width:620px;margin:32px auto;padding:0 18px;line-height:1.45}"
    "button{padding:12px 16px;background:#155eef;color:white;border:0;border-radius:8px;font-size:16px}"
    "pre{white-space:pre-wrap;background:#f2f4f7;padding:12px;border-radius:8px}</style>"
    "<h2>GeoLocator secure browser verification</h2>"
    "<p>This test checks trusted HTTPS, camera access and WebXR/immersive AR capability on this browser.</p>"
    "<button id=run>Verify phone</button>"
    "<pre id=result>Waiting for verification...</pre>"
    "<script>"
    "const out=document.getElementById('result');"
    "const btn=document.getElementById('run');"
    "async function verify(){"
      "btn.disabled=true;out.textContent='Checking...';"
      "const secure_context=window.isSecureContext===true;"
      "const camera_api=!!(navigator.mediaDevices&&navigator.mediaDevices.getUserMedia);"
      "let camera=false;"
      "if(camera_api){try{const s=await navigator.mediaDevices.getUserMedia({video:{facingMode:{ideal:'environment'}},audio:false});camera=true;s.getTracks().forEach(t=>t.stop());}catch(e){camera=false;}}"
      "const xr_api=!!(navigator.xr&&navigator.xr.isSessionSupported);"
      "let immersive_ar=false;"
      "if(xr_api){try{immersive_ar=await navigator.xr.isSessionSupported('immersive-ar');}catch(e){immersive_ar=false;}}"
      "const report={secure_context,camera,camera_api,xr_api,immersive_ar,user_agent:navigator.userAgent};"
      "let server=null;"
      "try{const r=await fetch('/api/setup/verify'+location.search,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(report)});server=await r.json();}catch(e){server={ready:false,error:String(e)};}"
      "out.textContent=JSON.stringify({report,server},null,2);"
      "btn.disabled=false;"
    "}"
    "btn.addEventListener('click',verify);"
    "</script>";


static esp_err_t web_onboarding_send_html(
    httpd_req_t *request,
    const char *page
)
{
    if (
        request == NULL ||
        page == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    httpd_resp_set_type(
        request,
        "text/html; charset=utf-8"
    );

    httpd_resp_set_hdr(
        request,
        "Cache-Control",
        "no-store"
    );

    return httpd_resp_send(
        request,
        page,
        HTTPD_RESP_USE_STRLEN
    );
}


static bool web_onboarding_query_is_installed(
    httpd_req_t *request
)
{
    if (request == NULL) {
        return false;
    }

    const size_t query_length =
        httpd_req_get_url_query_len(request);

    if (
        query_length == 0 ||
        query_length >= WEB_ONBOARDING_QUERY_SIZE
    ) {
        return false;
    }

    char query[WEB_ONBOARDING_QUERY_SIZE];

    if (
        httpd_req_get_url_query_str(
            request,
            query,
            sizeof(query)
        ) != ESP_OK
    ) {
        return false;
    }

    char installed[8];

    if (
        httpd_query_key_value(
            query,
            "installed",
            installed,
            sizeof(installed)
        ) != ESP_OK
    ) {
        return false;
    }

    return strcmp(installed, "1") == 0;
}


static esp_err_t web_onboarding_ca_page_handler(
    httpd_req_t *request
)
{
    if (web_onboarding_query_is_installed(request)) {
        s_ca_install_confirmed =
            true;

        ESP_LOGI(
            TAG,
            "Root CA installation confirmed by user"
        );

        (void)app_events_post(
            APP_EVENT_TLS_CA_INSTALL_CONFIRMED,
            0
        );

        return web_onboarding_send_html(
            request,
            s_ca_confirmed_page
        );
    }

    return web_onboarding_send_html(
        request,
        s_ca_page
    );
}


static esp_err_t web_onboarding_ca_file_handler(
    httpd_req_t *request
)
{
#if WEB_SERVER_ROOT_CA_EMBEDDED
    if (request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t length =
        (size_t)(
            root_ca_crt_end -
            root_ca_crt_start
        );

    if (
        length > 0 &&
        root_ca_crt_start[length - 1U] == '\0'
    ) {
        --length;
    }

    httpd_resp_set_type(
        request,
        "application/x-x509-ca-cert"
    );

    httpd_resp_set_hdr(
        request,
        "Content-Disposition",
        "attachment; filename=GeoLocator-Root-CA.crt"
    );

    httpd_resp_set_hdr(
        request,
        "Cache-Control",
        "no-store"
    );

    const esp_err_t err =
        httpd_resp_send(
            request,
            (const char *)root_ca_crt_start,
            (ssize_t)length
        );

    if (err != ESP_OK) {
        return err;
    }

    s_ca_downloaded =
        true;

    ESP_LOGI(
        TAG,
        "Public Root CA downloaded: %u bytes",
        (unsigned)length
    );

    (void)app_events_post(
        APP_EVENT_TLS_CA_DOWNLOADED,
        0
    );

    return ESP_OK;
#else
    static const char unavailable[] =
        "GeoLocator Root CA is not embedded in this firmware.";

    httpd_resp_set_status(
        request,
        "503 Service Unavailable"
    );

    httpd_resp_set_type(
        request,
        "text/plain; charset=utf-8"
    );

    return httpd_resp_send(
        request,
        unavailable,
        HTTPD_RESP_USE_STRLEN
    );
#endif
}


static bool web_onboarding_request_nonce(
    httpd_req_t *request,
    uint32_t *nonce_out
)
{
    if (
        request == NULL ||
        nonce_out == NULL
    ) {
        return false;
    }

    const size_t query_length =
        httpd_req_get_url_query_len(request);

    if (
        query_length == 0 ||
        query_length >= WEB_ONBOARDING_QUERY_SIZE
    ) {
        return false;
    }

    char query[WEB_ONBOARDING_QUERY_SIZE];

    if (
        httpd_req_get_url_query_str(
            request,
            query,
            sizeof(query)
        ) != ESP_OK
    ) {
        return false;
    }

    char token[WEB_ONBOARDING_TOKEN_SIZE];

    if (
        httpd_query_key_value(
            query,
            "t",
            token,
            sizeof(token)
        ) != ESP_OK
    ) {
        return false;
    }

    errno = 0;
    char *end = NULL;

    const unsigned long value =
        strtoul(
            token,
            &end,
            16
        );

    if (
        errno != 0 ||
        end == token ||
        *end != '\0' ||
        value > UINT32_MAX
    ) {
        return false;
    }

    *nonce_out =
        (uint32_t)value;

    return true;
}


static bool web_onboarding_nonce_matches(
    httpd_req_t *request
)
{
    uint32_t nonce = 0;

    return
        s_verify_nonce != 0 &&
        web_onboarding_request_nonce(
            request,
            &nonce
        ) &&
        nonce == s_verify_nonce;
}


static esp_err_t web_onboarding_verify_page_handler(
    httpd_req_t *request
)
{
    if (!web_onboarding_nonce_matches(request)) {
        return httpd_resp_send_err(
            request,
            HTTPD_403_FORBIDDEN,
            "Invalid or expired GeoLocator verification token"
        );
    }

    return web_onboarding_send_html(
        request,
        s_verify_page
    );
}


static esp_err_t web_onboarding_read_report(
    httpd_req_t *request,
    char *buffer,
    size_t buffer_size
)
{
    if (
        request == NULL ||
        buffer == NULL ||
        buffer_size < 2
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    if (
        request->content_len == 0 ||
        request->content_len >= buffer_size
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t received =
        0;

    while (received < request->content_len) {
        const int result =
            httpd_req_recv(
                request,
                buffer + received,
                request->content_len - received
            );

        if (result <= 0) {
            return ESP_FAIL;
        }

        received +=
            (size_t)result;
    }

    buffer[received] =
        '\0';

    return ESP_OK;
}


static bool web_onboarding_json_true(
    const cJSON *root,
    const char *name
)
{
    const cJSON *item =
        cJSON_GetObjectItemCaseSensitive(
            root,
            name
        );

    return cJSON_IsTrue(item);
}


static esp_err_t web_onboarding_verify_report_handler(
    httpd_req_t *request
)
{
    if (!web_onboarding_nonce_matches(request)) {
        return httpd_resp_send_err(
            request,
            HTTPD_403_FORBIDDEN,
            "Invalid or expired GeoLocator verification token"
        );
    }

    char body[WEB_ONBOARDING_REPORT_SIZE];

    esp_err_t err =
        web_onboarding_read_report(
            request,
            body,
            sizeof(body)
        );

    if (err != ESP_OK) {
        return httpd_resp_send_err(
            request,
            HTTPD_400_BAD_REQUEST,
            "Invalid verification report"
        );
    }

    cJSON *root =
        cJSON_Parse(body);

    if (root == NULL) {
        return httpd_resp_send_err(
            request,
            HTTPD_400_BAD_REQUEST,
            "Verification report is not valid JSON"
        );
    }

    uint32_t mask =
        0;

    if (
        web_onboarding_json_true(
            root,
            "secure_context"
        )
    ) {
        mask |= APP_TLS_VERIFY_SECURE_CONTEXT;
    }

    if (
        web_onboarding_json_true(
            root,
            "camera"
        )
    ) {
        mask |= APP_TLS_VERIFY_CAMERA;
    }

    if (
        web_onboarding_json_true(
            root,
            "xr_api"
        )
    ) {
        mask |= APP_TLS_VERIFY_XR_API;
    }

    if (
        web_onboarding_json_true(
            root,
            "immersive_ar"
        )
    ) {
        mask |= APP_TLS_VERIFY_IMMERSIVE_AR;
    }

    cJSON_Delete(root);

    const bool ready =
        (mask & APP_TLS_VERIFY_REQUIRED_MASK) ==
        APP_TLS_VERIFY_REQUIRED_MASK;

    ESP_LOGI(
        TAG,
        "Browser verification result: mask=0x%02x ready=%s",
        (unsigned)mask,
        ready ? "yes" : "no"
    );

    (void)app_events_post(
        APP_EVENT_TLS_VERIFY_RESULT,
        (int32_t)mask
    );

    char response[64];

    const int length =
        snprintf(
            response,
            sizeof(response),
            "{\"ready\":%s,\"mask\":%u}",
            ready ? "true" : "false",
            (unsigned)mask
        );

    if (
        length <= 0 ||
        length >= (int)sizeof(response)
    ) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(
        request,
        "application/json; charset=utf-8"
    );

    httpd_resp_set_hdr(
        request,
        "Cache-Control",
        "no-store"
    );

    return httpd_resp_send(
        request,
        response,
        length
    );
}


bool web_https_onboarding_ca_install_confirmed(void)
{
    return s_ca_install_confirmed;
}


esp_err_t web_https_onboarding_bootstrap_register(
    httpd_handle_t server
)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_ca_downloaded =
        false;

    s_ca_install_confirmed =
        false;

    const httpd_uri_t ca_page_uri = {
        .uri = "/setup/ca",
        .method = HTTP_GET,
        .handler = web_onboarding_ca_page_handler,
        .user_ctx = NULL,
    };

    esp_err_t err =
        web_server_register_uri_handler(
            server,
            &ca_page_uri
        );

    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t ca_file_uri = {
        .uri = "/setup/ca.crt",
        .method = HTTP_GET,
        .handler = web_onboarding_ca_file_handler,
        .user_ctx = NULL,
    };

    return web_server_register_uri_handler(
        server,
        &ca_file_uri
    );
}


esp_err_t web_https_onboarding_secure_register(
    httpd_handle_t server
)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_verify_nonce =
        esp_random();

    if (s_verify_nonce == 0) {
        s_verify_nonce =
            1;
    }

    const httpd_uri_t verify_page_uri = {
        .uri = "/setup/verify",
        .method = HTTP_GET,
        .handler = web_onboarding_verify_page_handler,
        .user_ctx = NULL,
    };

    esp_err_t err =
        web_server_register_uri_handler(
            server,
            &verify_page_uri
        );

    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t verify_report_uri = {
        .uri = "/api/setup/verify",
        .method = HTTP_POST,
        .handler = web_onboarding_verify_report_handler,
        .user_ctx = NULL,
    };

    err =
        web_server_register_uri_handler(
            server,
            &verify_report_uri
        );

    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(
        TAG,
        "HTTPS verifier ready: nonce=%08" PRIx32,
        s_verify_nonce
    );

    (void)app_events_post(
        APP_EVENT_TLS_VERIFY_READY,
        (int32_t)s_verify_nonce
    );

    return ESP_OK;
}
