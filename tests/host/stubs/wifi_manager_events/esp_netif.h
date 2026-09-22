#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct esp_netif_t {
    unsigned marker;
} esp_netif_t;

typedef struct {
    uint32_t addr;
} esp_ip4_addr_t;

typedef struct {
    esp_ip4_addr_t ip;
} esp_netif_ip_info_t;

typedef struct {
    uint8_t mac[6];
    esp_ip4_addr_t ip;
} ip_event_ap_staipassigned_t;

#define IPSTR "%u.%u.%u.%u"
#define IP2STR(address) \
    (unsigned)(((const esp_ip4_addr_t *)(address))->addr & 0xffU), \
    (unsigned)((((const esp_ip4_addr_t *)(address))->addr >> 8U) & 0xffU), \
    (unsigned)((((const esp_ip4_addr_t *)(address))->addr >> 16U) & 0xffU), \
    (unsigned)((((const esp_ip4_addr_t *)(address))->addr >> 24U) & 0xffU)

esp_err_t esp_netif_get_ip_info(
    esp_netif_t *netif,
    esp_netif_ip_info_t *ip_info
);
