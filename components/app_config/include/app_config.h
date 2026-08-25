#pragma once


/*
 * Fixed Wi-Fi configuration for the current prototype.
 *
 * These values will later be used by both the TFT UI and
 * the Wi-Fi SoftAP component.
 */
#define APP_WIFI_AP_SSID          "GEOLOCATOR"
#define APP_WIFI_AP_PASSWORD      "GEOLOCATOR"
#define APP_WIFI_AP_ADDRESS       "HTTP://192.168.4.1"


/*
 * Optional SD boot write/read diagnostic.
 *
 * 0:
 *   Mount the card at boot without creating a test file.
 *
 * 1:
 *   Create, verify and remove a temporary diagnostic file
 *   before the rest of the application starts.
 *
 * The normal prototype configuration keeps this disabled.
 */
#define APP_SD_BOOT_WRITE_TEST_ENABLED  0
