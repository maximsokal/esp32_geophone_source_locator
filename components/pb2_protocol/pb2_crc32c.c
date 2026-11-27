#include "pb2_protocol.h"

/* Standard Castagnoli CRC-32C (reversed polynomial 0x82F63B78). */
uint32_t pb2_crc32c(const void *data, size_t len) {
    if (data == NULL && len > 0U) {
        return 0U;
    }

    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFU;

    for (size_t i = 0U; i < len; ++i) {
        crc ^= (uint32_t)bytes[i];
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (0x82F63B78U & mask);
        }
    }

    return ~crc;
}
