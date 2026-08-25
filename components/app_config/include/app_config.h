#pragma once


/*
 * Fixed Wi-Fi configuration for the current prototype.
 *
 * These values are used by both the TFT UI and
 * the Wi-Fi SoftAP component.
 */
#define APP_WIFI_AP_SSID          "GEOLOCATOR"
#define APP_WIFI_AP_PASSWORD      "GEOLOCATOR"
#define APP_WIFI_AP_ADDRESS       "HTTP://192.168.4.1"


/*
 * Local-only Wi-Fi policy.
 *
 * When enabled, the SoftAP DHCP server does not advertise
 * ESP32 as the client's default router. Clients keep direct
 * access to 192.168.4.0/24 while their operating system may
 * continue using cellular data for Internet traffic.
 */
#define APP_WIFI_LOCAL_ONLY_ENABLED    1


/*
 * Captive portal policy.
 *
 * Keep this disabled in normal local-only operation so DNS
 * requests are not redirected to the ESP32. It can be enabled
 * later for a dedicated setup/demo mode if needed.
 */
#define APP_CAPTIVE_PORTAL_ENABLED     0


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
