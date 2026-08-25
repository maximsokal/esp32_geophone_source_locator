#pragma once

#include "esp_err.h"

#include "esp_http_server.h"


typedef enum
{
    WEB_STATIC_ASSET_INDEX_HTML = 0,
    WEB_STATIC_ASSET_DASHBOARD_CSS,
    WEB_STATIC_ASSET_DASHBOARD_JS,
    WEB_STATIC_ASSET_SEISMIC_HTML,
    WEB_STATIC_ASSET_SEISMIC_CSS,
    WEB_STATIC_ASSET_SEISMIC_JS,

    WEB_STATIC_ASSET_COUNT

} web_static_asset_id_t;


const char *web_static_asset_data(
    web_static_asset_id_t asset
);


esp_err_t web_static_assets_register(
    httpd_handle_t server
);
