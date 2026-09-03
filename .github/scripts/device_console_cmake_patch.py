from pathlib import Path

path = Path('components/web_server/CMakeLists.txt')
text = path.read_text(encoding='utf-8')
original = text

replacements = [
    (
        '        file(GLOB WEB_SERVER_WEBUI_CHUNK_SOURCES\n'
        '            "${WEB_SERVER_WEBUI_SOURCE_DIR}/assets/*.js.gz"\n'
        '        )',
        '        file(GLOB WEB_SERVER_WEBUI_CHUNK_SOURCES\n'
        '            "${WEB_SERVER_WEBUI_SOURCE_DIR}/assets/*.js.gz"\n'
        '            "${WEB_SERVER_WEBUI_SOURCE_DIR}/assets/*.css.gz"\n'
        '        )',
    ),
    (
        '            if(WEB_SERVER_WEBUI_CHUNK_NAME STREQUAL "app.js.gz")\n'
        '                continue()\n'
        '            endif()',
        '            if(\n'
        '                WEB_SERVER_WEBUI_CHUNK_NAME STREQUAL "app.js.gz"\n'
        '                OR WEB_SERVER_WEBUI_CHUNK_NAME STREQUAL "app.css.gz"\n'
        '            )\n'
        '                continue()\n'
        '            endif()',
    ),
    (
        '                "GeoLocator WebUI split build produced no secondary JS chunks"',
        '                "GeoLocator WebUI split build produced no secondary JS/CSS assets"',
    ),
    (
        '        "web_api_status.c"\n'
        '        "web_client_log.c"',
        '        "web_api_status.c"\n'
        '        "web_api_control.c"\n'
        '        "web_client_log.c"',
    ),
    (
        '        "web_status_json.c"\n'
        '        "web_status_json_pods.c"',
        '        "web_status_json.c"\n'
        '        "web_status_json_objects.c"\n'
        '        "web_status_json_pods.c"',
    ),
    (
        '        "web_status_json_objects.c"\n'
        '        "web_status_json_pods.c"',
        '        "web_status_json_objects.c"\n'
        '        "web_status_json_recording.c"\n'
        '        "web_status_json_pods.c"',
    ),
    (
        '        device_status\n'
        '        sd_card\n'
        '        seismic_buffer',
        '        device_status\n'
        '        object_store\n'
        '        sd_card\n'
        '        seismic_acquisition\n'
        '        seismic_buffer',
    ),
    (
        '        seismic_acquisition\n'
        '        seismic_buffer\n'
        '        wifi_manager',
        '        seismic_acquisition\n'
        '        seismic_buffer\n'
        '        seismic_recorder\n'
        '        wifi_manager',
    ),
]

for old, new in replacements:
    if new in text:
        continue
    if old not in text:
        raise SystemExit(f'Expected CMake fragment not found:\n{old}')
    text = text.replace(old, new, 1)

if text != original:
    path.write_text(text, encoding='utf-8')
    print('Patched web_server CMake wiring.')
else:
    print('web_server CMake wiring already patched.')
