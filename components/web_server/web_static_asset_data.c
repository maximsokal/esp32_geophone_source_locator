#include "web_static_assets.h"

#include <stddef.h>
#include <stdint.h>


extern const uint8_t index_html_start[]
    asm("_binary_index_html_start");

extern const uint8_t dashboard_css_start[]
    asm("_binary_dashboard_css_start");

extern const uint8_t dashboard_js_start[]
    asm("_binary_dashboard_js_start");

extern const uint8_t seismic_html_start[]
    asm("_binary_seismic_html_start");

extern const uint8_t seismic_css_start[]
    asm("_binary_seismic_css_start");

extern const uint8_t seismic_js_start[]
    asm("_binary_seismic_js_start");


const char *web_static_asset_data(
    web_static_asset_id_t asset
)
{
    switch (asset) {
        case WEB_STATIC_ASSET_INDEX_HTML:
            return
                (const char *)
                    index_html_start;

        case WEB_STATIC_ASSET_DASHBOARD_CSS:
            return
                (const char *)
                    dashboard_css_start;

        case WEB_STATIC_ASSET_DASHBOARD_JS:
            return
                (const char *)
                    dashboard_js_start;

        case WEB_STATIC_ASSET_SEISMIC_HTML:
            return
                (const char *)
                    seismic_html_start;

        case WEB_STATIC_ASSET_SEISMIC_CSS:
            return
                (const char *)
                    seismic_css_start;

        case WEB_STATIC_ASSET_SEISMIC_JS:
            return
                (const char *)
                    seismic_js_start;

        case WEB_STATIC_ASSET_COUNT:
        default:
            return NULL;
    }
}