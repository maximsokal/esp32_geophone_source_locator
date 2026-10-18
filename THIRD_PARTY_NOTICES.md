# Third-Party Notices

This project includes third-party components and assets listed below. They remain under their own licenses; the repository's Public Domain / CC0 dedication does not apply to them. The inventory reflects the firmware and WebUI audit reports and the package versions verified in the local checkout. It lists identified direct packages, managed components, bundled SDK code, and build/test tools; it is not a complete recursive inventory of every transitive dependency.

## Firmware: ESP-IDF and managed components

| Component | Version | SPDX license | Upstream | How used |
|---|---|---|---|---|
| ESP-IDF | 5.5.4 | Apache-2.0 | [espressif/esp-idf](https://github.com/espressif/esp-idf) | External framework and toolchain dependency; firmware build/runtime |
| espressif/button | 4.2.0 | Apache-2.0 | [esp-iot-solution/button](https://github.com/espressif/esp-iot-solution/tree/master/components/button) | Direct dependency; keypad and power-control firmware |
| espressif/cbor | 0.6.1~4 | MIT | [idf-extra-components/cbor](https://github.com/espressif/idf-extra-components/tree/master/cbor) | Transitive ESP Insights dependency; Insights is disabled in the current configuration |
| espressif/cmake_utilities | 0.5.3 | Apache-2.0 | [esp-iot-solution/cmake_utilities](https://github.com/espressif/esp-iot-solution/tree/master/tools/cmake_utilities) | Transitive build helper |
| espressif/esp-dsp | 1.8.2 | Apache-2.0 | [espressif/esp-dsp](https://github.com/espressif/esp-dsp) | Declared but unused; kept intentionally: pinned by the architecture contract test (`tests/python/test_architecture_refactor_contract.py`). No ESP-DSP API calls or `esp_dsp.h` includes were found |
| espressif/esp_diag_data_store | 1.1.1 | Apache-2.0 | [esp-insights/esp_diag_data_store](https://github.com/espressif/esp-insights/tree/main/components/esp_diag_data_store) | Transitive ESP Insights dependency; Insights is disabled in the current configuration |
| espressif/esp_diagnostics | 1.3.4 | Apache-2.0 | [esp-insights/esp_diagnostics](https://github.com/espressif/esp-insights/tree/main/components/esp_diagnostics) | Transitive ESP Insights dependency; Insights is disabled in the current configuration |
| espressif/esp_insights | 1.3.4 | Apache-2.0 | [esp-insights/esp_insights](https://github.com/espressif/esp-insights/tree/main/components/esp_insights) | Optional; disabled in `sdkconfig` (lines 631 and 2507); integration is guarded by `CONFIG_APP_ESP_INSIGHTS` |
| espressif/esp_lcd_ili9341 | 2.0.2 | Apache-2.0 | [esp-bsp/esp_lcd_ili9341](https://github.com/espressif/esp-bsp/tree/master/components/lcd/esp_lcd_ili9341) | Direct TFT display driver dependency; firmware |
| espressif/esp_lvgl_port | 2.9.0 | Apache-2.0 | [esp-bsp/esp_lvgl_port](https://github.com/espressif/esp-bsp/tree/master/components/esp_lvgl_port) | Direct LVGL display integration dependency; firmware |
| espressif/i2c_bus | 1.5.2 | Apache-2.0 | [esp-iot-solution/i2c_bus](https://github.com/espressif/esp-iot-solution/tree/master/components/i2c_bus) | Direct sensor-bus dependency; firmware |
| espressif/led_indicator | 2.1.2 | Apache-2.0 | [esp-iot-solution/led_indicator](https://github.com/espressif/esp-iot-solution/tree/master/components/led/led_indicator) | Direct status-indicator dependency; current board backend is disabled |
| espressif/led_strip | 3.0.3 | Apache-2.0 | [idf-extra-components/led_strip](https://github.com/espressif/idf-extra-components/tree/master/led_strip) | Transitive LED dependency |
| espressif/mdns | 1.11.3 | Apache-2.0 | [esp-protocols/mdns](https://github.com/espressif/esp-protocols/tree/master/components/mdns) | Direct dependency; firmware Wi-Fi service discovery |
| espressif/rmaker_cmd_resp | 1.0.0 | Apache-2.0 | [esp-rainmaker-common/rmaker_cmd_resp](https://github.com/espressif/esp-rainmaker-common/tree/master/components/rmaker_cmd_resp) | Transitive ESP Insights dependency; Insights is disabled in the current configuration |
| espressif/rmaker_common | 1.8.6 | Apache-2.0 | [esp-rainmaker-common](https://github.com/espressif/esp-rainmaker-common) | Transitive ESP Insights dependency; Insights is disabled in the current configuration |
| espressif/rmaker_common_events | 1.0.0 | Apache-2.0 | [esp-rainmaker-common/rmaker_common_events](https://github.com/espressif/esp-rainmaker-common/tree/master/components/rmaker_common_events) | Transitive ESP Insights dependency; Insights is disabled in the current configuration |
| espressif/rmaker_console | 1.0.1 | Apache-2.0 | [esp-rainmaker-common/rmaker_console](https://github.com/espressif/esp-rainmaker-common/tree/master/components/rmaker_console) | Transitive ESP Insights dependency; Insights is disabled in the current configuration |
| espressif/rmaker_system_ctrl | 1.0.0 | Apache-2.0 | [esp-rainmaker-common/rmaker_system_ctrl](https://github.com/espressif/esp-rainmaker-common/tree/master/components/rmaker_system_ctrl) | Transitive ESP Insights dependency; Insights is disabled in the current configuration |
| espressif/rmaker_time_sync | 1.0.2 | Apache-2.0 | [esp-rainmaker-common/rmaker_time_sync](https://github.com/espressif/esp-rainmaker-common/tree/master/components/rmaker_time_sync) | Transitive ESP Insights dependency; Insights is disabled in the current configuration |
| espressif/rmaker_work_queue | 1.0.0 | Apache-2.0 | [esp-rainmaker-common/rmaker_work_queue](https://github.com/espressif/esp-rainmaker-common/tree/master/components/rmaker_work_queue) | Transitive ESP Insights dependency; Insights is disabled in the current configuration |
| lvgl/lvgl | 9.6.0~1 | MIT | [lvgl/lvgl](https://github.com/lvgl/lvgl) | Direct UI dependency; bundled in firmware |

The target-test lockfiles also use `espressif/esp_lcd_ili9341` 2.1.0 (Apache-2.0; stress-app test build) and `espressif/cmake_utilities` 1.1.1 (Apache-2.0; test-app build). These are test-only versions; the production versions above remain 2.0.2 and 0.5.3.

## Code bundled by ESP-IDF

These components are supplied by the ESP-IDF 5.5.4 installation. The audit identified them as SDK code used or configured by the firmware; optional diagnostics may not be active in the current configuration.

| Component | Version | SPDX license | Upstream | How used |
|---|---|---|---|---|
| cJSON | 1.7.19 | MIT | [DaveGamble/cJSON](https://github.com/DaveGamble/cJSON) | JSON implementation bundled in firmware |
| FreeRTOS-Kernel | 10.5.1 | MIT | [FreeRTOS-Kernel](https://github.com/FreeRTOS/FreeRTOS-Kernel) | Task and queue runtime bundled in firmware |
| Mbed TLS | 3.6.5 | Apache-2.0 OR GPL-2.0-or-later | [Mbed-TLS/mbedtls](https://github.com/Mbed-TLS/mbedtls) | ESP-TLS/HTTPS support bundled in firmware |
| lwIP | 2.2.0 development snapshot | BSD-3-Clause | [lwip-tcpip/lwip](https://github.com/lwip-tcpip/lwip) | Network stack bundled in firmware |
| FatFs | R0.15 patch 2 | BSD-3-Clause | [Elm-Chan FatFs](http://elm-chan.org/fsw/ff/) | SD-card filesystem bundled in firmware |
| http-parser | 2.7.0 | MIT | [nodejs/http-parser](https://github.com/nodejs/http-parser) | HTTP server parser bundled in firmware |
| wpa_supplicant | 2.10 | BSD-3-Clause | [w1.fi/wpa_supplicant](https://w1.fi/wpa_supplicant/) | Wi-Fi authentication bundled in firmware |
| protobuf-c | 1.4.1 | MIT | [protobuf-c](https://github.com/protobuf-c/protobuf-c) | Optional diagnostics/ESP Insights dependency |
| SPIFFS | 0.2-255-g0dbb3f71c5f6 (submodule hash `0dbb3f71c5f6fae3747a9d935372773762baf852`) | MIT | [pellepl/spiffs](https://github.com/pellepl/spiffs) | Configured filesystem support; revision reported by [Espressif ESP-IDF v5.5.4 `.gitmodules`](https://github.com/espressif/esp-idf/blob/v5.5.4/.gitmodules), not verified locally |
| ESP-IDF Wi-Fi and PHY binary libraries | 5.5.4 | Apache-2.0 | [ESP-IDF components](https://github.com/espressif/esp-idf/tree/v5.5.4/components) | Binary SDK libraries bundled in firmware |

## nRF54L15 / nRF Connect SDK

| Component | Version | SPDX license | Upstream | How used |
|---|---|---|---|---|
| nRF Connect SDK / sdk-nrf | 3.4.0 | LicenseRef-Nordic-5-Clause | [nrfconnect/sdk-nrf](https://github.com/nrfconnect/sdk-nrf) | External SDK dependency for nRF54L15 firmware |
| NCS Zephyr fork | ncs-v3.4.0 / Zephyr 4.4.0 | Apache-2.0 | [nrfconnect/sdk-zephyr](https://github.com/nrfconnect/sdk-zephyr) | Kernel and Bluetooth stack dependency |
| nrfxlib / SoftDevice Controller | 3.4.0 | LicenseRef-Nordic-5-Clause | [nrfconnect/sdk-nrfxlib](https://github.com/nrfconnect/sdk-nrfxlib) | Binary controller selected for nRF firmware |
| hal_nordic | Revision 18da0cc9726f8759c627dba3180b3ba9294e433c | BSD-3-Clause for nrfx sources; LicenseRef-Nordic-5-Clause for the module | [zephyrproject-rtos/hal_nordic](https://github.com/zephyrproject-rtos/hal_nordic) | Peripheral driver dependency; licensing varies by source/module |
| CMSIS | Revision 512cc7e895e8491696b61f7ba8066b4a182569b8 | Apache-2.0 | [zephyrproject-rtos/cmsis](https://github.com/zephyrproject-rtos/cmsis) | Cortex-M core support dependency |

The Nordic license files are provided with the matching nRF Connect SDK release. `LicenseRef-Nordic-5-Clause` is retained as the SDK's license identifier; files in `hal_nordic` can carry their own SPDX license, as shown above.

## Firmware assets and tools

| Component | Version | SPDX license | Upstream | How used |
|---|---|---|---|---|
| LVGL built-in QR widget | LVGL 9.6.0~1 | MIT | [lvgl/lvgl](https://github.com/lvgl/lvgl) | QR display dependency bundled in firmware |
| QR-Code-generator by Project Nayuki, bundled with LVGL | No separate version recorded; included with LVGL 9.6.0~1 | MIT | [Nayuki QR Code Generator](https://www.nayuki.io/page/qr-code-generator-library) | Vendored QR implementation bundled in firmware; license at `managed_components/lvgl__lvgl/src/libs/qrcode/LICENSE.txt` |
| Montserrat Regular (vendored TTF and generated LVGL C font) | TTF version 9.000; source commit `cc8daf2e7085006b9c112542fc82b58afc13521d`; TTF SHA-256 `3e8abe50c44c82e2242e97d1ec8c0d385c4890cdc50447bcdb8605c81a38cfb2`; generated 14 px font | OFL-1.1 | [JulietaUla/Montserrat](https://github.com/JulietaUla/Montserrat) | Vendored font converted to generated C and bundled in firmware; license at `components/ui/fonts/OFL.txt` |
| LVGL built-in Montserrat / Font Awesome symbol glyphs | LVGL 9.6.0~1; Font Awesome release not recorded | OFL-1.1 for font software; CC-BY-4.0 for Font Awesome artwork if distributed as SVG/JS | [LVGL](https://github.com/lvgl/lvgl), [Font Awesome](https://fontawesome.com) | Symbol glyphs bundled in firmware; no SVG/JS artwork was identified in the audit |
| lv_font_conv | 1.5.3 | MIT (upstream package metadata; local license source not present) | [lvgl/lv_font_conv](https://github.com/lvgl/lv_font_conv) | Build-only font converter for the vendored Montserrat font |
| AprilTag `tag36h11` family/codeword data | No local version recorded; dictionary copy is from `js-aruco2` 2.0.0 | BSD-2-Clause | [AprilRobotics/apriltag-generation `Tag36h11.java`](https://github.com/AprilRobotics/apriltag-generation/blob/master/src/april/tag/Tag36h11.java) | Dictionary bundled in WebUI firmware assets and source for converted print-sheet tooling data; attribution follows below |
| ESP-IDF Component Manager (`idf_component_tools`) | 2.4.11 in the firmware audit; not pinned by the project | Apache-2.0 | [espressif/idf-component-manager](https://github.com/espressif/idf-component-manager) | Build/developer helper used by a host preparation script |
| Segno | 1.6.6 in generator instructions; not installed in the audit environment | BSD-3-Clause | [heuer/segno](https://github.com/heuer/segno) | Build-only QR golden-fixture generator |
| pytest | 9.0.1 in the audit environment; not pinned by the project | MIT | [pytest-dev/pytest](https://github.com/pytest-dev/pytest) | Test-only Python runner |
| Seeed PlatformIO platform | Commit 1ec1287f8e4bc4067a6fd593991e36875aef989f | Apache-2.0 | [Seeed-Studio/platform-seeedboards](https://github.com/Seeed-Studio/platform-seeedboards) | Build-only; optional legacy smoke build |
| Espressif esp-emu (esp-emulator) | v0.49.0 | Apache-2.0 | [espressif/esp-emulator](https://github.com/espressif/esp-emulator); [LICENSE](https://raw.githubusercontent.com/espressif/esp-emulator/main/LICENSE) | Dev/test-only tool used by `tools/run_emulator_smoke.py` inside WSL. The prebuilt Linux x86_64 binary is downloaded from the project's GitHub release with SHA-256 verification; it is not bundled in firmware, stored in the repository, or modified |
| WSL2 Ubuntu 26.04 and west | Ubuntu 26.04; west version not specified | Not individually listed | [Ubuntu](https://ubuntu.com/), [west](https://github.com/zephyrproject-rtos/west) | Dev-only host tooling for `native_sim`: WSL2 Ubuntu 26.04 with apt packages `gcc`, `cmake`, `ninja`, `dtc`, `python3-venv`, and west |
| RBJ Audio EQ Cookbook formulas | Not applicable | N/A (reference only; no imported implementation found) | [W3C Audio EQ Cookbook](https://www.w3.org/TR/audio-eq-cookbook/) | Formula reference only; no third-party implementation code identified |

## KiCad design files

- `kicad/XIAO_Series_SCH_Symbols/`: Seeed Studio XIAO symbol library (third-party).
- `kicad/202004288;XIAO nRF54L15 v1.0_SCH&PCB/`: Seeed reference design (third-party).
- `kicad/old/`: archived project copy referencing the Seeed library.
- `kicad/esp32_geophone_source_locator/`: project design using those Seeed symbols.

No license or copyright notice was found in the Seeed files. The only license-like text is `CC BY-SA 4.0` in the reference design's `fp-info-cache`, near a `Logo` record; it does not clearly cover the Seeed files. The Seeed library and reference-design license is **UNVERIFIED** and must be confirmed with Seeed Studio before redistribution. The board references external 3D models by path; those model files are not in this tree.

## WebUI npm dependencies

Installed versions and SPDX values below were checked against local `webui/node_modules` package metadata and the audit report. “Web bundle” means the production WebUI assets are staged for firmware embedding.

The 19 production packages in `webui/package-lock.json` are 18 MIT and one BSD-3-Clause (`ieee754`); no GPL, LGPL, AGPL, or unknown licenses were found. The existing production WebUI assets do not include `ieee754`, so it is not listed as a bundled component.

| Component | Version | SPDX license | Upstream | How used |
|---|---|---|---|---|
| `@react-three/fiber` | 9.7.0 | MIT | [pmndrs/react-three-fiber](https://github.com/pmndrs/react-three-fiber) | 3D views; bundled in WebUI firmware assets |
| `js-aruco2` | 2.0.0 | MIT | [damianofalcioni/js-aruco2](https://github.com/damianofalcioni/js-aruco2) | AprilTag detector and dictionary; bundled in WebUI firmware assets; dictionary also feeds the print-sheet tooling |
| `react` | 19.2.8 | MIT | [react/react](https://github.com/react/react) | UI runtime; bundled in WebUI firmware assets |
| `react-dom` | 19.2.8 | MIT | [react/react](https://github.com/react/react) | Browser root and report markup; bundled in WebUI firmware assets |
| `three` | 0.185.1 | MIT | [mrdoob/three.js](https://github.com/mrdoob/three.js) | 3D rendering and controls; bundled in WebUI firmware assets |
| `@fontsource/montserrat` | 5.3.0 | OFL-1.1 | [fontsource/font-files: Montserrat](https://github.com/fontsource/font-files/tree/main/fonts/google/montserrat) | WOFF2 Latin/Cyrillic subsets inlined into the report print chunk; bundled in WebUI firmware assets |
| `@types/react` | 19.2.18 | MIT | [DefinitelyTyped/types/react](https://github.com/DefinitelyTyped/DefinitelyTyped/tree/master/types/react) | Compiler-only type declarations; not in firmware |
| `@types/react-dom` | 19.2.5 | MIT | [DefinitelyTyped/types/react-dom](https://github.com/DefinitelyTyped/DefinitelyTyped/tree/master/types/react-dom) | Compiler-only type declarations; not in firmware |
| `@types/three` | 0.185.4 | MIT | [DefinitelyTyped/types/three](https://github.com/DefinitelyTyped/DefinitelyTyped/tree/master/types/three) | Compiler-only type declarations; not in firmware |
| `@vitejs/plugin-react` | 6.1.1 | MIT | [vitejs/vite-plugin-react](https://github.com/vitejs/vite-plugin-react) | Build-only Vite plugin |
| `@vitest/coverage-v8` | 4.1.11 | MIT | [vitest-dev/vitest](https://github.com/vitest-dev/vitest) | Test-only coverage provider |
| `fontkit` | 2.0.4 | MIT | [foliojs/fontkit](https://github.com/foliojs/fontkit) | Test-only font inspection |
| `react-test-renderer` | 19.2.8 | MIT | [react/react](https://github.com/react/react) | Test-only React renderer |
| `typescript` | 7.0.2 | Apache-2.0 | [microsoft/TypeScript](https://github.com/microsoft/TypeScript) | Build/type-check only |
| `vite` | 8.2.2 | MIT | [vitejs/vite](https://github.com/vitejs/vite) | Development server and production bundler; build-only in distributed firmware |
| `vitest` | 4.1.11 | MIT | [vitest-dev/vitest](https://github.com/vitest-dev/vitest) | Test-only runner |

## Required attribution and license references

### Montserrat — SIL Open Font License 1.1

Copyright 2024 The Montserrat.Git Project Authors (https://github.com/JulietaUla/Montserrat.git).

The full OFL-1.1 text is retained at [`components/ui/fonts/OFL.txt`](components/ui/fonts/OFL.txt). The OFL allows font software to be bundled with software when the copyright notice and license accompany each copy. Under clause 3, a Modified Version may not use a Reserved Font Name without written permission from the corresponding copyright holder. Keep the OFL notice with the vendored font and generated font software.

### AprilTag `tag36h11` data — BSD-2-Clause

The following notice was verified in `webui/node_modules/js-aruco2/src/dictionaries/apriltag_36h11.js`. The dictionary identifies its source as [AprilRobotics/apriltag-generation `Tag36h11.java`](https://github.com/AprilRobotics/apriltag-generation/blob/master/src/april/tag/Tag36h11.java).

```text
Copyright (C) 2013-2016, The Regents of The University of Michigan.
All rights reserved.
This software was developed in the APRIL Robotics Lab under the
direction of Edwin Olson, ebolson@umich.edu. This software may be
available under alternative licensing terms; contact the address above.
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
The views and conclusions contained in the software and documentation are those
of the authors and should not be interpreted as representing official policies,
either expressed or implied, of the Regents of The University of Michigan.
```

The same `tag36h11` family data is used by the WebUI dictionary and the print-sheet generator. The generated `tag_patterns.gen.json` is tooling output and does not contain its own copy of this attribution; distribute this notice with it.

### MIT components

The table identifies the MIT-licensed React/Three.js packages, other MIT npm packages, and MIT firmware components such as cJSON, FreeRTOS-Kernel, http-parser, protobuf-c, SPIFFS, LVGL, Nayuki QR-Code-generator, and espressif/cbor. Their package/source distributions provide their respective MIT copyright and permission notices; retain those notices when redistributing the corresponding source or binaries. `js-aruco2` code is MIT-licensed; its AprilTag `tag36h11` data has the separate BSD-2-Clause notice above.

### Apache-2.0 components

ESP-IDF and the listed Espressif managed components use Apache-2.0 where shown in the tables. Their license texts are supplied with the ESP-IDF installation or the corresponding managed component. The tables keep bundled libraries' own MIT, BSD, or dual-license terms separate from the framework license.

### Nordic-licensed components

The nRF Connect SDK and nrfxlib / SoftDevice Controller entries use `LicenseRef-Nordic-5-Clause`. The matching Nordic license files are included with the nRF Connect SDK distribution. The `hal_nordic` module contains both Nordic-licensed module material and BSD-3-Clause nrfx source files.

## Own work

Original project work includes the firmware in `components/`, `main/`, and `nRF54L15/src/` and `nRF54L15/include/`; the WebUI in `webui/src/`; and the supporting code in `tools/`, `tests/`, and `i18n/`. The KiCad project `kicad/esp32_geophone_source_locator/` is an original project design apart from its Seeed symbols. The firmware audit identified 60 original component directories. This includes the AD7606, INA3221, TCA9548A, BMI160, and BME280 register-level drivers, localization and seismic processing, storage, and application services. The audit found no imported third-party sensor-driver library, Kalman implementation, or FFT implementation. The QR code renderer in `webui/src/domain/qrCode.ts` is also project code; the RBJ formulas are a reference only.

## Open items

- Confirm the Seeed symbol-library and reference-design license with Seeed Studio before redistributing; it remains **UNVERIFIED**. External 3D models are referenced by path and are not in this tree.
- The embedded Montserrat TTF reports version 9.000; the WebUI `@fontsource/montserrat` package is separately identified as version 5.3.0.
- The SPIFFS revision is reported by Espressif's ESP-IDF v5.5.4 `.gitmodules` and was not verified locally.
- Dev-only transitive npm dependencies were not individually license-audited. The production lockfile set contains 19 packages (18 MIT, one BSD-3-Clause `ieee754`) with no GPL, LGPL, AGPL, or unknown licenses. The complete transitive ESP-IDF managed-component dependency tree was also outside the audit scope.
- The license of the esp-emu release binary is assumed to match the repository license; this was not separately verified.
- `lv_font_conv` is identified as MIT by upstream package metadata, but its source/license was not present locally in the firmware audit.
