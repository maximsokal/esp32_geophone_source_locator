#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"


void wifi_manager_runtime_set_setup_client(
    const uint8_t mac[6],
    bool known
);

void wifi_manager_runtime_clear_setup_client(void);

bool wifi_manager_runtime_setup_client_connected(void);

bool wifi_manager_runtime_setup_client_known(void);

esp_err_t wifi_manager_runtime_setup_client_mac(
    uint8_t mac[6]
);

esp_err_t wifi_manager_runtime_setup_client_mac_string(
    char *text,
    size_t text_size
);


void wifi_manager_runtime_set_sta_ip(
    const esp_ip4_addr_t *ip
);

void wifi_manager_runtime_clear_sta_ip(void);

esp_err_t wifi_manager_runtime_sta_ip_string(
    char *text,
    size_t text_size
);
