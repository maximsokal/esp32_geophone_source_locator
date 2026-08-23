#include "web_page.h"

#include "app_config.h"


static const char s_index_html[] =
    "<!DOCTYPE html>"
    "<html lang=\"en\">"

    "<head>"
        "<meta charset=\"utf-8\">"
        "<meta name=\"viewport\" "
        "content=\"width=device-width,initial-scale=1\">"

        "<title>GeoLocator</title>"

        "<style>"
            "*{box-sizing:border-box;}"

            "body{"
                "margin:0;"
                "font-family:Arial,sans-serif;"
                "background:#0d1117;"
                "color:#e6edf3;"
            "}"

            ".page{"
                "max-width:720px;"
                "margin:0 auto;"
                "padding:24px;"
            "}"

            ".title{"
                "font-size:30px;"
                "font-weight:700;"
                "margin-bottom:6px;"
            "}"

            ".subtitle{"
                "color:#8b949e;"
                "margin-bottom:28px;"
            "}"

            ".card{"
                "background:#161b22;"
                "border:1px solid #30363d;"
                "border-radius:12px;"
                "padding:20px;"
                "margin-bottom:16px;"
            "}"

            ".card-title{"
                "font-size:18px;"
                "font-weight:700;"
                "margin-bottom:16px;"
            "}"

            ".row{"
                "display:flex;"
                "justify-content:space-between;"
                "gap:16px;"
                "padding:9px 0;"
                "border-bottom:1px solid #21262d;"
            "}"

            ".row:last-child{"
                "border-bottom:none;"
            "}"

            ".label{"
                "color:#8b949e;"
            "}"

            ".value{"
                "font-weight:600;"
                "text-align:right;"
            "}"

            ".ok{"
                "color:#3fb950;"
            "}"

            ".pending{"
                "color:#d29922;"
            "}"

            ".footer{"
                "margin-top:26px;"
                "color:#8b949e;"
                "font-size:13px;"
                "text-align:center;"
            "}"
        "</style>"
    "</head>"

    "<body>"
        "<main class=\"page\">"

            "<div class=\"title\">GeoLocator</div>"

            "<div class=\"subtitle\">"
                "ESP32-S3 Geophone Source Locator"
            "</div>"

            "<section class=\"card\">"

                "<div class=\"card-title\">Connection</div>"

                "<div class=\"row\">"
                    "<span class=\"label\">Wi-Fi</span>"
                    "<span class=\"value ok\">CONNECTED</span>"
                "</div>"

                "<div class=\"row\">"
                    "<span class=\"label\">SSID</span>"
                    "<span class=\"value\">"
                        APP_WIFI_AP_SSID
                    "</span>"
                "</div>"

                "<div class=\"row\">"
                    "<span class=\"label\">Address</span>"
                    "<span class=\"value\">192.168.4.1</span>"
                "</div>"

            "</section>"

            "<section class=\"card\">"

                "<div class=\"card-title\">Device</div>"

                "<div class=\"row\">"
                    "<span class=\"label\">System</span>"
                    "<span class=\"value ok\">READY</span>"
                "</div>"

                "<div class=\"row\">"
                    "<span class=\"label\">Power monitor</span>"
                    "<span class=\"value pending\">WAITING FOR INA3221</span>"
                "</div>"

                "<div class=\"row\">"
                    "<span class=\"label\">Sensor pods</span>"
                    "<span class=\"value pending\">NOT CONNECTED</span>"
                "</div>"

            "</section>"

            "<section class=\"card\">"

                "<div class=\"card-title\">Measurement</div>"

                "<div class=\"row\">"
                    "<span class=\"label\">AD7606</span>"
                    "<span class=\"value pending\">NOT CONNECTED</span>"
                "</div>"

                "<div class=\"row\">"
                    "<span class=\"label\">Recording</span>"
                    "<span class=\"value\">IDLE</span>"
                "</div>"

            "</section>"

            "<div class=\"footer\">"
                "Local interface running directly on ESP32-S3"
            "</div>"

        "</main>"
    "</body>"

    "</html>";


const char *web_page_get_index_html(void)
{
    return s_index_html;
}