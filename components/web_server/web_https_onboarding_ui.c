#include "web_server_internal.h"

#include <stddef.h>

#include "app_event_bus.h"


#if WEB_SERVER_ROOT_CA_EMBEDDED
extern const unsigned char root_ca_crt_start[]
    asm("_binary_rootCA_crt_start");
extern const unsigned char root_ca_crt_end[]
    asm("_binary_rootCA_crt_end");
#endif


static const char s_ca_guide_page[] =
    "<!doctype html>"
    "<html><head>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>GeoLocator Root CA setup</title>"
    "<style>"
    "body{font-family:system-ui,sans-serif;max-width:680px;margin:28px auto;padding:0 18px;line-height:1.5;color:#111827}"
    "h1{font-size:28px;line-height:1.15;margin-bottom:10px}"
    ".card{padding:16px;border-radius:12px;background:#f3f4f6;margin:16px 0}"
    ".warn{background:#fff7ed;border:1px solid #fed7aa}"
    ".ok{background:#ecfdf3;border:1px solid #a7f3d0}"
    "a.btn{display:block;text-align:center;padding:14px 16px;margin:14px 0;border-radius:10px;text-decoration:none;font-weight:700}"
    ".primary{background:#155eef;color:white}"
    ".success{background:#067647;color:white}"
    "ol{padding-left:24px}li{margin:10px 0}code{word-break:break-all}"
    "</style></head><body>"
    "<h1>GeoLocator HTTPS certificate</h1>"
    "<div class='card warn'><b>Android security limitation:</b> Chrome cannot install a CA certificate directly from this page. If Android says that CA certificates must be installed from Settings, that is expected.</div>"
    "<ol>"
    "<li>Download the public GeoLocator Root CA file.</li>"
    "<li>Open <b>Settings</b> on the phone.</li>"
    "<li>On Samsung / One UI open: <b>Security and privacy &rarr; More security settings &rarr; Credential storage &rarr; Install from device storage &rarr; CA certificate</b>.</li>"
    "<li>If your Android menus differ, search Settings for <b>CA certificate</b> or <b>Install certificate</b>.</li>"
    "<li>Select <code>GeoLocator-Root-CA.crt</code> from Downloads and confirm the Android warning / <b>Install anyway</b>.</li>"
    "<li>Return to this page and press the green button below.</li>"
    "</ol>"
    "<a class='btn primary' href='/setup/ca-download'>1. Download certificate file</a>"
    "<div class='card ok'>Only the <b>public</b> Root CA certificate is downloaded. The Root CA private key never leaves the development computer.</div>"
    "<a class='btn success' href='/setup/ca?installed=1'>2. I installed the CA in Settings - continue</a>"
    "</body></html>";


static const char s_verify_ui_page[] =
    "<!doctype html>"
    "<html><head>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>GeoLocator secure browser verification</title>"
    "<style>"
    "body{font-family:system-ui,sans-serif;max-width:680px;margin:28px auto;padding:0 18px;line-height:1.45;color:#111827}"
    "h1{font-size:28px;line-height:1.15}"
    "button,a.btn{display:inline-block;padding:14px 18px;border:0;border-radius:10px;font-size:17px;font-weight:700;text-decoration:none}"
    "button{background:#155eef;color:white}button:disabled{opacity:.55}"
    ".status{margin:20px 0;padding:18px;border-radius:14px;background:#f3f4f6;border:1px solid #e5e7eb}"
    ".status.ok{background:#ecfdf3;border-color:#6ce9a6}.status.bad{background:#fff7ed;border-color:#fdba74}"
    ".status h2{margin:0 0 8px}.check{font-size:17px;margin:8px 0}.yes{color:#067647}.no{color:#b54708}"
    ".dashboard{background:#067647;color:white;margin-top:10px}"
    "details{margin-top:18px}pre{white-space:pre-wrap;word-break:break-word;background:#f3f4f6;padding:12px;border-radius:8px;font-size:12px}"
    "</style></head><body>"
    "<h1>GeoLocator secure browser verification</h1>"
    "<p>The phone will verify trusted HTTPS, real camera access, WebXR API availability and immersive AR support.</p>"
    "<button id=run>Verify phone</button>"
    "<section id=status class=status><h2>Ready to verify</h2><p>Press <b>Verify phone</b> and allow camera access when Android asks.</p></section>"
    "<details id=details hidden><summary>Technical details</summary><pre id=technical></pre></details>"
    "<script>"
    "const statusBox=document.getElementById('status');"
    "const btn=document.getElementById('run');"
    "const details=document.getElementById('details');"
    "const technical=document.getElementById('technical');"
    "function item(label,value){return `<div class='check ${value?'yes':'no'}'>${value?'&#10003;':'&#10007;'} ${label}: <b>${value?'PASS':'FAIL'}</b></div>`;}"
    "async function verify(){"
      "btn.disabled=true;btn.textContent='Checking...';"
      "statusBox.className='status';statusBox.innerHTML='<h2>Verification in progress...</h2><p>Checking HTTPS, camera and WebXR capabilities.</p>';"
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
      "const ready=!!(server&&server.ready);"
      "let checks=item('Trusted secure HTTPS context',secure_context)+item('Camera access',camera)+item('WebXR API',xr_api)+item('Immersive AR',immersive_ar);"
      "if(ready){"
        "statusBox.className='status ok';"
        "statusBox.innerHTML='<h2>&#10003; Verification completed successfully</h2><p><b>GeoLocator secure browser setup is complete.</b> This phone is ready for the browser-only camera/WebXR workflow.</p>'+checks+\"<a class='btn dashboard' href='/'>Open GeoLocator dashboard</a>\";"
        "btn.textContent='Verified';"
      "}else{"
        "statusBox.className='status bad';"
        "statusBox.innerHTML='<h2>Verification incomplete</h2><p>One or more required checks did not pass. Review the failed item below and try again.</p>'+checks;"
        "btn.disabled=false;btn.textContent='Verify again';"
      "}"
      "technical.textContent=JSON.stringify({report,server},null,2);details.hidden=false;"
    "}"
    "btn.addEventListener('click',verify);"
    "</script></body></html>";


static esp_err_t web_onboarding_ui_send_html(
    httpd_req_t *request,
    const char *page
)
{
    if (request == NULL || page == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");

    return httpd_resp_send(
        request,
        page,
        HTTPD_RESP_USE_STRLEN
    );
}


static esp_err_t web_onboarding_ca_guide_handler(
    httpd_req_t *request
)
{
    return web_onboarding_ui_send_html(
        request,
        s_ca_guide_page
    );
}


static esp_err_t web_onboarding_ca_download_handler(
    httpd_req_t *request
)
{
#if WEB_SERVER_ROOT_CA_EMBEDDED
    if (request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t length = (size_t)(
        root_ca_crt_end - root_ca_crt_start
    );

    if (
        length > 0 &&
        root_ca_crt_start[length - 1U] == '\0'
    ) {
        --length;
    }

    /*
     * Deliberately use a generic binary download type. Android/Chrome may
     * otherwise launch the certificate installer directly, but modern
     * Android requires CA installation to be initiated from Settings.
     */
    httpd_resp_set_type(
        request,
        "application/octet-stream"
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

    const esp_err_t err = httpd_resp_send(
        request,
        (const char *)root_ca_crt_start,
        (ssize_t)length
    );

    if (err == ESP_OK) {
        (void)app_event_bus_publish(
            APP_EVENT_TLS_CA_DOWNLOADED,
            0
        );
    }

    return err;
#else
    return httpd_resp_send_err(
        request,
        HTTPD_500_INTERNAL_SERVER_ERROR,
        "GeoLocator Root CA is not embedded"
    );
#endif
}


static esp_err_t web_onboarding_verify_ui_handler(
    httpd_req_t *request
)
{
    return web_onboarding_ui_send_html(
        request,
        s_verify_ui_page
    );
}


esp_err_t web_https_onboarding_ui_bootstrap_register(
    httpd_handle_t server
)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const httpd_uri_t guide_uri = {
        .uri = "/setup/ca-guide",
        .method = HTTP_GET,
        .handler = web_onboarding_ca_guide_handler,
        .user_ctx = NULL,
    };

    esp_err_t err = httpd_register_uri_handler(
        server,
        &guide_uri
    );

    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t download_uri = {
        .uri = "/setup/ca-download",
        .method = HTTP_GET,
        .handler = web_onboarding_ca_download_handler,
        .user_ctx = NULL,
    };

    return httpd_register_uri_handler(
        server,
        &download_uri
    );
}


esp_err_t web_https_onboarding_ui_secure_register(
    httpd_handle_t server
)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const httpd_uri_t verify_ui_uri = {
        .uri = "/setup/verify-ui",
        .method = HTTP_GET,
        .handler = web_onboarding_verify_ui_handler,
        .user_ctx = NULL,
    };

    return httpd_register_uri_handler(
        server,
        &verify_ui_uri
    );
}
