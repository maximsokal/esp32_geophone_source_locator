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
            "*{"
                "box-sizing:border-box;"
            "}"

            "body{"
                "margin:0;"
                "font-family:Arial,sans-serif;"
                "background:#0d1117;"
                "color:#e6edf3;"
            "}"

            ".page{"
                "max-width:1100px;"
                "margin:0 auto;"
                "padding:20px;"
            "}"

            ".header{"
                "display:flex;"
                "justify-content:space-between;"
                "align-items:flex-start;"
                "gap:16px;"
                "margin-bottom:22px;"
            "}"

            ".title{"
                "font-size:30px;"
                "font-weight:700;"
                "margin-bottom:5px;"
            "}"

            ".subtitle{"
                "color:#8b949e;"
                "font-size:14px;"
            "}"

            ".api-state{"
                "padding:7px 11px;"
                "border:1px solid #30363d;"
                "border-radius:20px;"
                "font-size:12px;"
                "font-weight:700;"
                "white-space:nowrap;"
            "}"

            ".grid{"
                "display:grid;"
                "grid-template-columns:"
                    "repeat(2,minmax(0,1fr));"
                "gap:16px;"
            "}"

            ".card{"
                "background:#161b22;"
                "border:1px solid #30363d;"
                "border-radius:12px;"
                "padding:18px;"
                "min-width:0;"
            "}"

            ".wide{"
                "grid-column:1 / -1;"
            "}"

            ".card-title{"
                "font-size:17px;"
                "font-weight:700;"
                "margin-bottom:14px;"
            "}"

            ".row{"
                "display:flex;"
                "justify-content:space-between;"
                "gap:16px;"
                "padding:8px 0;"
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
                "font-variant-numeric:tabular-nums;"
            "}"

            ".ok{"
                "color:#3fb950;"
            "}"

            ".warning{"
                "color:#d29922;"
            "}"

            ".error{"
                "color:#f85149;"
            "}"

            ".muted{"
                "color:#8b949e;"
            "}"

            ".table-wrap{"
                "overflow-x:auto;"
            "}"

            "table{"
                "width:100%;"
                "border-collapse:collapse;"
                "font-size:14px;"
            "}"

            "th{"
                "color:#8b949e;"
                "font-weight:600;"
                "text-align:right;"
                "padding:8px;"
                "border-bottom:1px solid #30363d;"
            "}"

            "th:first-child{"
                "text-align:left;"
            "}"

            "td{"
                "padding:9px 8px;"
                "text-align:right;"
                "border-bottom:1px solid #21262d;"
                "font-variant-numeric:tabular-nums;"
            "}"

            "td:first-child{"
                "text-align:left;"
                "font-weight:600;"
            "}"

            "tr:last-child td{"
                "border-bottom:none;"
            "}"

            ".pods{"
                "display:grid;"
                "grid-template-columns:"
                    "repeat(2,minmax(0,1fr));"
                "gap:12px;"
            "}"

            ".pod{"
                "border:1px solid #30363d;"
                "border-radius:10px;"
                "padding:14px;"
                "min-width:0;"
            "}"

            ".pod-header{"
                "display:flex;"
                "justify-content:space-between;"
                "align-items:center;"
                "gap:12px;"
                "margin-bottom:10px;"
            "}"

            ".pod-title{"
                "font-weight:700;"
                "font-size:16px;"
            "}"

            ".pod-state{"
                "font-size:12px;"
                "font-weight:700;"
            "}"

            ".section-label{"
                "margin-top:12px;"
                "margin-bottom:4px;"
                "font-size:12px;"
                "font-weight:700;"
                "color:#8b949e;"
                "text-transform:uppercase;"
                "letter-spacing:.05em;"
            "}"

            ".footer{"
                "margin-top:24px;"
                "color:#8b949e;"
                "font-size:12px;"
                "text-align:center;"
            "}"

            "@media(max-width:760px){"
                ".grid{"
                    "grid-template-columns:1fr;"
                "}"

                ".wide{"
                    "grid-column:auto;"
                "}"

                ".pods{"
                    "grid-template-columns:1fr;"
                "}"

                ".header{"
                    "align-items:center;"
                "}"

                ".page{"
                    "padding:14px;"
                "}"
            "}"
        "</style>"
    "</head>"

    "<body>"

        "<main class=\"page\">"

            "<div class=\"header\">"

                "<div>"
                    "<div class=\"title\">GeoLocator</div>"
                    "<div class=\"subtitle\">"
                        "ESP32-S3 Geophone Source Locator"
                    "</div>"
                "</div>"

                "<div "
                    "id=\"api-state\" "
                    "class=\"api-state warning\""
                ">"
                    "CONNECTING"
                "</div>"

            "</div>"

            "<div class=\"grid\">"

                "<section class=\"card\">"

                    "<div class=\"card-title\">System</div>"

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

                    "<div class=\"row\">"
                        "<span class=\"label\">System state</span>"
                        "<span "
                            "id=\"system-state\" "
                            "class=\"value muted\""
                        ">"
                            "--"
                        "</span>"
                    "</div>"

                    "<div class=\"row\">"
                        "<span class=\"label\">Mode</span>"
                        "<span "
                            "id=\"system-mode\" "
                            "class=\"value\""
                        ">"
                            "--"
                        "</span>"
                    "</div>"

                    "<div class=\"row\">"
                        "<span class=\"label\">Uptime</span>"
                        "<span "
                            "id=\"system-uptime\" "
                            "class=\"value\""
                        ">"
                            "--"
                        "</span>"
                    "</div>"

                "</section>"


                "<section class=\"card\">"

                    "<div class=\"card-title\">Environment</div>"

                    "<div class=\"row\">"
                        "<span class=\"label\">Temperature</span>"
                        "<span "
                            "id=\"env-temperature\" "
                            "class=\"value\""
                        ">"
                            "--"
                        "</span>"
                    "</div>"

                    "<div class=\"row\">"
                        "<span class=\"label\">Humidity</span>"
                        "<span "
                            "id=\"env-humidity\" "
                            "class=\"value\""
                        ">"
                            "--"
                        "</span>"
                    "</div>"

                    "<div class=\"row\">"
                        "<span class=\"label\">Pressure</span>"
                        "<span "
                            "id=\"env-pressure\" "
                            "class=\"value\""
                        ">"
                            "--"
                        "</span>"
                    "</div>"

                "</section>"


                "<section class=\"card wide\">"

                    "<div class=\"card-title\">Power / INA3221</div>"

                    "<div class=\"row\">"
                        "<span class=\"label\">Main voltage</span>"
                        "<span "
                            "id=\"power-voltage\" "
                            "class=\"value\""
                        ">"
                            "--"
                        "</span>"
                    "</div>"

                    "<div class=\"row\">"
                        "<span class=\"label\">Main current</span>"
                        "<span "
                            "id=\"power-current\" "
                            "class=\"value\""
                        ">"
                            "--"
                        "</span>"
                    "</div>"

                    "<div class=\"row\">"
                        "<span class=\"label\">Main power</span>"
                        "<span "
                            "id=\"power-watts\" "
                            "class=\"value\""
                        ">"
                            "--"
                        "</span>"
                    "</div>"

                    "<div class=\"table-wrap\">"

                        "<table>"

                            "<thead>"
                                "<tr>"
                                    "<th>Channel</th>"
                                    "<th>Status</th>"
                                    "<th>Voltage</th>"
                                    "<th>Current</th>"
                                    "<th>Power</th>"
                                "</tr>"
                            "</thead>"

                            "<tbody>"

                                "<tr>"
                                    "<td>Channel 1</td>"
                                    "<td id=\"power-ch1-status\">--</td>"
                                    "<td id=\"power-ch1-v\">--</td>"
                                    "<td id=\"power-ch1-a\">--</td>"
                                    "<td id=\"power-ch1-w\">--</td>"
                                "</tr>"

                                "<tr>"
                                    "<td>Channel 2</td>"
                                    "<td id=\"power-ch2-status\">--</td>"
                                    "<td id=\"power-ch2-v\">--</td>"
                                    "<td id=\"power-ch2-a\">--</td>"
                                    "<td id=\"power-ch2-w\">--</td>"
                                "</tr>"

                                "<tr>"
                                    "<td>Channel 3</td>"
                                    "<td id=\"power-ch3-status\">--</td>"
                                    "<td id=\"power-ch3-v\">--</td>"
                                    "<td id=\"power-ch3-a\">--</td>"
                                    "<td id=\"power-ch3-w\">--</td>"
                                "</tr>"

                            "</tbody>"

                        "</table>"

                    "</div>"

                "</section>"


                "<section class=\"card\">"

                    "<div class=\"card-title\">I2C Multiplexer</div>"

                    "<div class=\"row\">"
                        "<span class=\"label\">TCA9548A</span>"
                        "<span "
                            "id=\"mux-status\" "
                            "class=\"value muted\""
                        ">"
                            "--"
                        "</span>"
                    "</div>"

                    "<div class=\"row\">"
                        "<span class=\"label\">Active channel</span>"
                        "<span "
                            "id=\"mux-channel\" "
                            "class=\"value\""
                        ">"
                            "--"
                        "</span>"
                    "</div>"

                    "<div class=\"row\">"
                        "<span class=\"label\">Active pod</span>"
                        "<span "
                            "id=\"mux-pod\" "
                            "class=\"value\""
                        ">"
                            "--"
                        "</span>"
                    "</div>"

                "</section>"


                "<section class=\"card\">"

                    "<div class=\"card-title\">Acquisition</div>"

                    "<div class=\"row\">"
                        "<span class=\"label\">Recording</span>"
                        "<span "
                            "id=\"acq-running\" "
                            "class=\"value\""
                        ">"
                            "--"
                        "</span>"
                    "</div>"

                    "<div class=\"row\">"
                        "<span class=\"label\">Sample rate</span>"
                        "<span "
                            "id=\"acq-rate\" "
                            "class=\"value\""
                        ">"
                            "--"
                        "</span>"
                    "</div>"

                    "<div class=\"row\">"
                        "<span class=\"label\">Samples</span>"
                        "<span "
                            "id=\"acq-samples\" "
                            "class=\"value\""
                        ">"
                            "--"
                        "</span>"
                    "</div>"

                    "<div class=\"row\">"
                        "<span class=\"label\">Recording time</span>"
                        "<span "
                            "id=\"acq-time\" "
                            "class=\"value\""
                        ">"
                            "--"
                        "</span>"
                    "</div>"

                "</section>"


                "<section class=\"card wide\">"

                    "<div class=\"card-title\">Sensor Pods</div>"

                    "<div class=\"pods\">"


                        "<div class=\"pod\">"

                            "<div class=\"pod-header\">"
                                "<div class=\"pod-title\">Pod 1</div>"
                                "<div "
                                    "id=\"pod1-state\" "
                                    "class=\"pod-state muted\""
                                ">"
                                    "--"
                                "</div>"
                            "</div>"

                            "<div class=\"section-label\">BMP580</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">Pressure</span>"
                                "<span id=\"pod1-pressure\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">Height</span>"
                                "<span id=\"pod1-height\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"section-label\">BMI160</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">Roll</span>"
                                "<span id=\"pod1-roll\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">Pitch</span>"
                                "<span id=\"pod1-pitch\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">Yaw rel.</span>"
                                "<span id=\"pod1-yaw\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"section-label\">Geophone</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">Instant</span>"
                                "<span id=\"pod1-instant\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">RMS</span>"
                                "<span id=\"pod1-rms\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">Peak</span>"
                                "<span id=\"pod1-peak\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">Noise</span>"
                                "<span id=\"pod1-noise\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">SNR</span>"
                                "<span id=\"pod1-snr\" class=\"value\">--</span>"
                            "</div>"

                        "</div>"


                        "<div class=\"pod\">"

                            "<div class=\"pod-header\">"
                                "<div class=\"pod-title\">Pod 2</div>"
                                "<div "
                                    "id=\"pod2-state\" "
                                    "class=\"pod-state muted\""
                                ">"
                                    "--"
                                "</div>"
                            "</div>"

                            "<div class=\"section-label\">BMP580</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">Pressure</span>"
                                "<span id=\"pod2-pressure\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">Height</span>"
                                "<span id=\"pod2-height\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"section-label\">BMI160</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">Roll</span>"
                                "<span id=\"pod2-roll\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">Pitch</span>"
                                "<span id=\"pod2-pitch\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">Yaw rel.</span>"
                                "<span id=\"pod2-yaw\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"section-label\">Geophone</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">Instant</span>"
                                "<span id=\"pod2-instant\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">RMS</span>"
                                "<span id=\"pod2-rms\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">Peak</span>"
                                "<span id=\"pod2-peak\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">Noise</span>"
                                "<span id=\"pod2-noise\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">SNR</span>"
                                "<span id=\"pod2-snr\" class=\"value\">--</span>"
                            "</div>"

                        "</div>"


                        "<div class=\"pod\">"

                            "<div class=\"pod-header\">"
                                "<div class=\"pod-title\">Pod 3</div>"
                                "<div "
                                    "id=\"pod3-state\" "
                                    "class=\"pod-state muted\""
                                ">"
                                    "--"
                                "</div>"
                            "</div>"

                            "<div class=\"section-label\">BMP580</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">Pressure</span>"
                                "<span id=\"pod3-pressure\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">Height</span>"
                                "<span id=\"pod3-height\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"section-label\">BMI160</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">Roll</span>"
                                "<span id=\"pod3-roll\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">Pitch</span>"
                                "<span id=\"pod3-pitch\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">Yaw rel.</span>"
                                "<span id=\"pod3-yaw\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"section-label\">Geophone</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">Instant</span>"
                                "<span id=\"pod3-instant\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">RMS</span>"
                                "<span id=\"pod3-rms\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">Peak</span>"
                                "<span id=\"pod3-peak\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">Noise</span>"
                                "<span id=\"pod3-noise\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">SNR</span>"
                                "<span id=\"pod3-snr\" class=\"value\">--</span>"
                            "</div>"

                        "</div>"


                        "<div class=\"pod\">"

                            "<div class=\"pod-header\">"
                                "<div class=\"pod-title\">Pod 4</div>"
                                "<div "
                                    "id=\"pod4-state\" "
                                    "class=\"pod-state muted\""
                                ">"
                                    "--"
                                "</div>"
                            "</div>"

                            "<div class=\"section-label\">BMP580</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">Pressure</span>"
                                "<span id=\"pod4-pressure\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">Height</span>"
                                "<span id=\"pod4-height\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"section-label\">BMI160</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">Roll</span>"
                                "<span id=\"pod4-roll\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">Pitch</span>"
                                "<span id=\"pod4-pitch\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">Yaw rel.</span>"
                                "<span id=\"pod4-yaw\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"section-label\">Geophone</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">Instant</span>"
                                "<span id=\"pod4-instant\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">RMS</span>"
                                "<span id=\"pod4-rms\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">Peak</span>"
                                "<span id=\"pod4-peak\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">Noise</span>"
                                "<span id=\"pod4-noise\" class=\"value\">--</span>"
                            "</div>"

                            "<div class=\"row\">"
                                "<span class=\"label\">SNR</span>"
                                "<span id=\"pod4-snr\" class=\"value\">--</span>"
                            "</div>"

                        "</div>"

                    "</div>"

                "</section>"

            "</div>"

            "<div class=\"footer\">"
                "Live data from ESP32-S3 /api/status"
            "</div>"

        "</main>"


        "<script>"

            "function el(id){"
                "return document.getElementById(id);"
            "}"


            "function text(id,value){"
                "var node=el(id);"
                "if(node){"
                    "node.textContent=value;"
                "}"
            "}"


            "function fixed(value,digits,suffix,valid){"
                "if(!valid){"
                    "return '--';"
                "}"

                "return Number(value).toFixed(digits)+suffix;"
            "}"


            "function formatUptime(ms){"
                "var total=Math.floor(ms/1000);"
                "var days=Math.floor(total/86400);"
                "var hours=Math.floor((total%86400)/3600);"
                "var minutes=Math.floor((total%3600)/60);"
                "var seconds=total%60;"

                "if(days>0){"
                    "return days+'d '+hours+'h '+minutes+'m';"
                "}"

                "if(hours>0){"
                    "return hours+'h '+minutes+'m '+seconds+'s';"
                "}"

                "return minutes+'m '+seconds+'s';"
            "}"


            "function setClass(id,className){"
                "var node=el(id);"
                "if(node){"
                    "node.className=className;"
                "}"
            "}"


            "function podStateClass(state){"
                "if(state==='READY'){"
                    "return 'pod-state ok';"
                "}"

                "if(state==='WARNING'){"
                    "return 'pod-state warning';"
                "}"

                "if(state==='ERROR'){"
                    "return 'pod-state error';"
                "}"

                "return 'pod-state muted';"
            "}"


            "function updateSystem(system){"
                "text("
                    "'system-state',"
                    "system.state"
                ");"

                "text("
                    "'system-mode',"
                    "system.mode"
                ");"

                "text("
                    "'system-uptime',"
                    "formatUptime(system.uptime_ms)"
                ");"

                "setClass("
                    "'system-state',"
                    "'value ok'"
                ");"
            "}"


            "function updatePower(power){"
                "text("
                    "'power-voltage',"
                    "fixed("
                        "power.voltage_v,"
                        "2,"
                        "' V',"
                        "power.valid"
                    ")"
                ");"

                "text("
                    "'power-current',"
                    "fixed("
                        "power.current_a,"
                        "3,"
                        "' A',"
                        "power.valid"
                    ")"
                ");"

                "text("
                    "'power-watts',"
                    "fixed("
                        "power.power_w,"
                        "2,"
                        "' W',"
                        "power.valid"
                    ")"
                ");"


                "for("
                    "var i=0;"
                    "i<power.channels.length && i<3;"
                    "i++"
                "){"
                    "var channel=power.channels[i];"
                    "var n=i+1;"
                    "var prefix='power-ch'+n;"

                    "text("
                        "prefix+'-status',"
                        "channel.valid?'OK':'--'"
                    ");"

                    "text("
                        "prefix+'-v',"
                        "fixed("
                            "channel.voltage_v,"
                            "2,"
                            "' V',"
                            "channel.valid"
                        ")"
                    ");"

                    "text("
                        "prefix+'-a',"
                        "fixed("
                            "channel.current_a,"
                            "3,"
                            "' A',"
                            "channel.valid"
                        ")"
                    ");"

                    "text("
                        "prefix+'-w',"
                        "fixed("
                            "channel.power_w,"
                            "2,"
                            "' W',"
                            "channel.valid"
                        ")"
                    ");"
                "}"
            "}"


            "function updateEnvironment(environment){"
                "text("
                    "'env-temperature',"
                    "fixed("
                        "environment.temperature_c,"
                        "1,"
                        "' C',"
                        "environment.valid"
                    ")"
                ");"

                "text("
                    "'env-humidity',"
                    "fixed("
                        "environment.humidity_percent,"
                        "1,"
                        "' %',"
                        "environment.valid"
                    ")"
                ");"

                "text("
                    "'env-pressure',"
                    "fixed("
                        "environment.pressure_pa,"
                        "1,"
                        "' Pa',"
                        "environment.valid"
                    ")"
                ");"
            "}"


            "function updateMux(mux){"
                "text("
                    "'mux-status',"
                    "mux.valid?'ACTIVE':'--'"
                ");"

                "setClass("
                    "'mux-status',"
                    "mux.valid?"
                        "'value ok':"
                        "'value muted'"
                ");"

                "text("
                    "'mux-channel',"
                    "mux.valid && mux.active_channel>=0?"
                        "String(mux.active_channel):"
                        "'--'"
                ");"

                "text("
                    "'mux-pod',"
                    "mux.valid && mux.active_pod>=0?"
                        "String(mux.active_pod+1):"
                        "'--'"
                ");"
            "}"


            "function updatePod(pod){"
                "var n=pod.id;"
                "var prefix='pod'+n;"

                "text("
                    "prefix+'-state',"
                    "pod.state"
                ");"

                "setClass("
                    "prefix+'-state',"
                    "podStateClass(pod.state)"
                ");"


                "var barometer=pod.barometer;"

                "text("
                    "prefix+'-pressure',"
                    "fixed("
                        "barometer.pressure_pa,"
                        "1,"
                        "' Pa',"
                        "barometer.valid"
                    ")"
                ");"

                "text("
                    "prefix+'-height',"
                    "fixed("
                        "barometer.relative_altitude_cm,"
                        "1,"
                        "' cm',"
                        "barometer.valid"
                    ")"
                ");"


                "var imu=pod.imu;"

                "text("
                    "prefix+'-roll',"
                    "fixed("
                        "imu.roll_deg,"
                        "1,"
                        "' deg',"
                        "imu.valid"
                    ")"
                ");"

                "text("
                    "prefix+'-pitch',"
                    "fixed("
                        "imu.pitch_deg,"
                        "1,"
                        "' deg',"
                        "imu.valid"
                    ")"
                ");"

                "text("
                    "prefix+'-yaw',"
                    "fixed("
                        "imu.yaw_relative_deg,"
                        "1,"
                        "' deg',"
                        "imu.valid"
                    ")"
                ");"


                "var geophone=pod.geophone;"

                "text("
                    "prefix+'-instant',"
                    "fixed("
                        "geophone.instant_uv,"
                        "1,"
                        "' uV',"
                        "geophone.valid"
                    ")"
                ");"

                "text("
                    "prefix+'-rms',"
                    "fixed("
                        "geophone.rms_uv,"
                        "1,"
                        "' uV',"
                        "geophone.valid"
                    ")"
                ");"

                "text("
                    "prefix+'-peak',"
                    "fixed("
                        "geophone.peak_uv,"
                        "1,"
                        "' uV',"
                        "geophone.valid"
                    ")"
                ");"

                "text("
                    "prefix+'-noise',"
                    "fixed("
                        "geophone.noise_rms_uv,"
                        "1,"
                        "' uV',"
                        "geophone.valid"
                    ")"
                ");"

                "text("
                    "prefix+'-snr',"
                    "fixed("
                        "geophone.snr_db,"
                        "1,"
                        "' dB',"
                        "geophone.valid"
                    ")"
                ");"
            "}"


            "function updatePods(pods){"
                "for("
                    "var i=0;"
                    "i<pods.length;"
                    "i++"
                "){"
                    "updatePod(pods[i]);"
                "}"
            "}"


            "function updateAcquisition(acquisition){"
                "text("
                    "'acq-running',"
                    "acquisition.running?"
                        "'RECORDING':"
                        "'IDLE'"
                ");"

                "setClass("
                    "'acq-running',"
                    "acquisition.running?"
                        "'value ok':"
                        "'value'"
                ");"

                "text("
                    "'acq-rate',"
                    "acquisition.sample_rate_hz>0?"
                        "acquisition.sample_rate_hz+' Hz':"
                        "'--'"
                ");"

                "text("
                    "'acq-samples',"
                    "String("
                        "acquisition.samples_recorded"
                    ")"
                ");"

                "text("
                    "'acq-time',"
                    "acquisition.recording_ms>0?"
                        "fixed("
                            "acquisition.recording_ms/1000,"
                            "1,"
                            "' s',"
                            "true"
                        "):"
                        "'--'"
                ");"
            "}"


            "function updateDashboard(data){"
                "updateSystem(data.system);"
                "updatePower(data.power);"
                "updateEnvironment(data.environment);"
                "updateMux(data.mux);"
                "updatePods(data.pods);"
                "updateAcquisition(data.acquisition);"
            "}"


            "function setApiOnline(online){"
                "if(online){"
                    "text("
                        "'api-state',"
                        "'LIVE'"
                    ");"

                    "setClass("
                        "'api-state',"
                        "'api-state ok'"
                    ");"
                "}"
                "else{"
                    "text("
                        "'api-state',"
                        "'OFFLINE'"
                    ");"

                    "setClass("
                        "'api-state',"
                        "'api-state error'"
                    ");"
                "}"
            "}"


            "function refreshStatus(){"
                "fetch("
                    "'/api/status',"
                    "{cache:'no-store'}"
                ")"
                ".then(function(response){"

                    "if(!response.ok){"
                        "throw new Error("
                            "'HTTP '+response.status"
                        ");"
                    "}"

                    "return response.json();"
                "})"
                ".then(function(data){"
                    "updateDashboard(data);"
                    "setApiOnline(true);"
                "})"
                ".catch(function(){"
                    "setApiOnline(false);"
                "});"
            "}"


            "refreshStatus();"

            "setInterval("
                "refreshStatus,"
                "1000"
            ");"

        "</script>"

    "</body>"

    "</html>";


const char *web_page_get_index_html(void)
{
    return s_index_html;
}