#include "wifi_manager.h"

#include "wifi_manager_runtime.h"


bool wifi_manager_setup_client_connected(void)
{
    return
        wifi_manager_runtime_setup_client_connected();
}


bool wifi_manager_setup_client_known(void)
{
    return
        wifi_manager_runtime_setup_client_known();
}


esp_err_t wifi_manager_setup_client_mac_string(
    char *text,
    size_t text_size
)
{
    return
        wifi_manager_runtime_setup_client_mac_string(
            text,
            text_size
        );
}


esp_err_t wifi_manager_sta_ip_string(
    char *text,
    size_t text_size
)
{
    return
        wifi_manager_runtime_sta_ip_string(
            text,
            text_size
        );
}
