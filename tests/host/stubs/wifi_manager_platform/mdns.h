#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t mdns_init(void);
esp_err_t mdns_hostname_set(const char *hostname);
esp_err_t mdns_instance_name_set(const char *instance_name);
esp_err_t mdns_service_add(
    const char *instance_name,
    const char *service,
    const char *protocol,
    uint16_t port,
    void *txt,
    size_t num_txt
);
void mdns_free(void);
