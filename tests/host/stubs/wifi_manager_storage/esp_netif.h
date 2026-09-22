#pragma once

#include <stdint.h>

typedef struct {
    uint32_t addr;
} esp_ip4_addr_t;

#define IPSTR "%u.%u.%u.%u"
#define IP2STR(address) \
    (unsigned)(((const esp_ip4_addr_t *)(address))->addr & 0xffU), \
    (unsigned)((((const esp_ip4_addr_t *)(address))->addr >> 8U) & 0xffU), \
    (unsigned)((((const esp_ip4_addr_t *)(address))->addr >> 16U) & 0xffU), \
    (unsigned)((((const esp_ip4_addr_t *)(address))->addr >> 24U) & 0xffU)
