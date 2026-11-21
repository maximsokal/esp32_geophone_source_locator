#pragma once

/*
 * SHA-256 of a scan file (issue #34), shared by the scan store and the localization. The scan identity "scan-crc32:xxxxxxxx" is a 32-bit CRC: it detects accidental corruption but two different
 * valid scans can share it, so a sidecar uploaded for scan A could be admitted for scan B. The phone sends the SHA-256 of the scan a sidecar was built
 * for (query scan_sha); the device compares it with the digest of the scan it holds.
 *
 * On the device the hardware-accelerated mbedTLS implementation is used; the host tests use the portable implementation below (same FIPS 180-4
 * result, checked against the standard vectors).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCAN_SHA256_BYTES 32U
#define SCAN_SHA256_HEX_CHARS 64U

#if defined(ESP_PLATFORM)

#include "mbedtls/sha256.h"

typedef struct {
    mbedtls_sha256_context ctx;
} scan_sha256_t;

static inline void scan_sha256_init(scan_sha256_t *state)
{
    mbedtls_sha256_init(&state->ctx);
    (void)mbedtls_sha256_starts(&state->ctx, 0);
}

static inline void scan_sha256_update(scan_sha256_t *state, const unsigned char *bytes, size_t length)
{
    if (length > 0U) (void)mbedtls_sha256_update(&state->ctx, bytes, length);
}

static inline void scan_sha256_final(scan_sha256_t *state, unsigned char out[SCAN_SHA256_BYTES])
{
    (void)mbedtls_sha256_finish(&state->ctx, out);
    mbedtls_sha256_free(&state->ctx);
}

/* Releases the context of a digest that will not be finished. */
static inline void scan_sha256_abort(scan_sha256_t *state) { mbedtls_sha256_free(&state->ctx); }

#else /* portable implementation for the host tests */

typedef struct {
    uint32_t h[8];
    unsigned char block[64];
    size_t block_length;
    uint64_t total_bytes;
} scan_sha256_t;

static const uint32_t SCAN_SHA256_K[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

static inline uint32_t scan_sha256_rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32U - n)); }

static inline void scan_sha256_compress(scan_sha256_t *state, const unsigned char *block)
{
    uint32_t w[64];
    for (size_t i = 0U; i < 16U; ++i) {
        w[i] = ((uint32_t)block[i * 4U] << 24U) | ((uint32_t)block[i * 4U + 1U] << 16U) |
            ((uint32_t)block[i * 4U + 2U] << 8U) | (uint32_t)block[i * 4U + 3U];
    }
    for (size_t i = 16U; i < 64U; ++i) {
        const uint32_t s0 = scan_sha256_rotr(w[i - 15U], 7U) ^ scan_sha256_rotr(w[i - 15U], 18U) ^ (w[i - 15U] >> 3U);
        const uint32_t s1 = scan_sha256_rotr(w[i - 2U], 17U) ^ scan_sha256_rotr(w[i - 2U], 19U) ^ (w[i - 2U] >> 10U);
        w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
    }
    uint32_t a = state->h[0], b = state->h[1], c = state->h[2], d = state->h[3];
    uint32_t e = state->h[4], f = state->h[5], g = state->h[6], h = state->h[7];
    for (size_t i = 0U; i < 64U; ++i) {
        const uint32_t s1 = scan_sha256_rotr(e, 6U) ^ scan_sha256_rotr(e, 11U) ^ scan_sha256_rotr(e, 25U);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + s1 + ch + SCAN_SHA256_K[i] + w[i];
        const uint32_t s0 = scan_sha256_rotr(a, 2U) ^ scan_sha256_rotr(a, 13U) ^ scan_sha256_rotr(a, 22U);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    state->h[0] += a; state->h[1] += b; state->h[2] += c; state->h[3] += d;
    state->h[4] += e; state->h[5] += f; state->h[6] += g; state->h[7] += h;
}

static inline void scan_sha256_init(scan_sha256_t *state)
{
    static const uint32_t initial[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    };
    for (size_t i = 0U; i < 8U; ++i) state->h[i] = initial[i];
    state->block_length = 0U;
    state->total_bytes = 0U;
}

static inline void scan_sha256_update(scan_sha256_t *state, const unsigned char *bytes, size_t length)
{
    state->total_bytes += (uint64_t)length;
    while (length > 0U) {
        size_t take = 64U - state->block_length;
        if (take > length) take = length;
        for (size_t i = 0U; i < take; ++i) state->block[state->block_length + i] = bytes[i];
        state->block_length += take;
        bytes += take;
        length -= take;
        if (state->block_length == 64U) {
            scan_sha256_compress(state, state->block);
            state->block_length = 0U;
        }
    }
}

/* Nothing to release in the portable implementation. */
static inline void scan_sha256_abort(scan_sha256_t *state) { (void)state; }

static inline void scan_sha256_final(scan_sha256_t *state, unsigned char out[SCAN_SHA256_BYTES])
{
    const uint64_t bit_length = state->total_bytes * 8U;
    state->block[state->block_length++] = 0x80U;
    if (state->block_length > 56U) {
        while (state->block_length < 64U) state->block[state->block_length++] = 0U;
        scan_sha256_compress(state, state->block);
        state->block_length = 0U;
    }
    while (state->block_length < 56U) state->block[state->block_length++] = 0U;
    for (size_t i = 0U; i < 8U; ++i) state->block[56U + i] = (unsigned char)(bit_length >> (56U - 8U * i));
    scan_sha256_compress(state, state->block);
    for (size_t i = 0U; i < 8U; ++i) {
        out[i * 4U] = (unsigned char)(state->h[i] >> 24U);
        out[i * 4U + 1U] = (unsigned char)(state->h[i] >> 16U);
        out[i * 4U + 2U] = (unsigned char)(state->h[i] >> 8U);
        out[i * 4U + 3U] = (unsigned char)state->h[i];
    }
}

#endif

/** Parses a lowercase 64-character hex digest; false for anything else. */
static inline bool scan_sha256_from_hex(const char *hex, unsigned char out[SCAN_SHA256_BYTES])
{
    if (hex == NULL) return false;
    size_t length = 0U;
    while (hex[length] != '\0' && length <= SCAN_SHA256_HEX_CHARS) ++length;
    if (length != SCAN_SHA256_HEX_CHARS) return false;
    for (size_t i = 0U; i < SCAN_SHA256_BYTES; ++i) {
        unsigned value = 0U;
        for (size_t digit = 0U; digit < 2U; ++digit) {
            const char c = hex[i * 2U + digit];
            unsigned nibble;
            if (c >= '0' && c <= '9') nibble = (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') nibble = (unsigned)(c - 'a') + 10U;
            else return false;
            value = (value << 4U) | nibble;
        }
        out[i] = (unsigned char)value;
    }
    return true;
}
