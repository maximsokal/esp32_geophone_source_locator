#pragma once


/*
 * Temporary setup SoftAP.
 */
#define APP_WIFI_AP_SSID          "GEOLOCATOR_SETUP"
#define APP_WIFI_AP_PASSWORD      "GEOLOCATOR"
#define APP_WIFI_SETUP_URL        "http://192.168.4.1/setup"


/*
 * Local-network discovery.
 *
 * The hostname intentionally excludes the .local suffix because the mDNS
 * responder appends it when publishing the host on the LAN.
 */
#define APP_MDNS_HOSTNAME         "geolocator"
#define APP_MDNS_INSTANCE_NAME    "GeoLocator"
#define APP_MDNS_WEB_URL          "http://geolocator.local/"


/*
 * Optional quick preset.
 *
 * This is only a convenience preset for the user's current Android phone.
 * It is not treated as a universal Android default.
 */
#define APP_WIFI_QUICK_SSID       "AndroidAP"
#define APP_WIFI_QUICK_PASSWORD   "1234567890"


#define APP_SD_BOOT_WRITE_TEST_ENABLED  0
