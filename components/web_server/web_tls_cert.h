#pragma once

/*
 * The board's HTTPS certificate, issued on the board for the address it has right now.
 *
 * A phone reaches the board by the address the router gave it ("https://192.168.0.191"), and a certificate made at build time can
 * only name "geolocator.local", so every browser connection by address failed the certificate check and the page's module
 * and API fetches failed with it (board 2026-11-17). The board therefore signs a leaf for its current addresses when HTTPS
 * starts. It signs with the name-constrained GeoLocator Device CA (tools/generate_geolocator_device_ca.py), never with the Root CA key.
 *
 * Pure mbedtls with the random source passed in, so a host test can check the result against openssl.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*web_tls_cert_rng_t)(void *context, unsigned char *buffer, size_t length);

typedef struct {
    const char *leaf_key_pem;           /* the server's private key, NUL-terminated PEM */
    const char *issuer_cert_pem;        /* the Device CA certificate, NUL-terminated PEM */
    const char *issuer_key_pem;         /* the Device CA private key, NUL-terminated PEM */
    const char *const *ipv4_addresses;  /* dotted text, e.g. "192.168.0.191"; NULL entries and "" are skipped */
    size_t ipv4_count;
    web_tls_cert_rng_t rng;
    void *rng_context;
} web_tls_cert_request_t;

/* DNS name every certificate carries (the mDNS name). */
#define WEB_TLS_CERT_DNS_NAME "geolocator.local"

/*
 * Writes "<leaf PEM><Device CA PEM>" (NUL-terminated) to out and returns 0, or a negative mbedtls code (-1 for bad arguments, -2 when
 * the buffer is too small, -3 when no usable address text was given and the name alone would repeat the build-time certificate).
 * out_length, when given, receives the length including the NUL (what the HTTPS server wants as servercert_len).
 */
int web_tls_cert_issue(const web_tls_cert_request_t *request, char *out, size_t out_size, size_t *out_length);

/* Parses "a.b.c.d"; true and the four bytes on success, nothing else accepted. */
int web_tls_cert_parse_ipv4(const char *text, unsigned char octets[4]);

#ifdef __cplusplus
}
#endif
