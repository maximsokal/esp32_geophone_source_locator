#include "web_tls_cert.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/asn1.h"
#include "mbedtls/md.h"
#include "mbedtls/oid.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509.h"
#include "mbedtls/x509_crt.h"

#define WEB_TLS_CERT_MAX_SAN_IPS 4U
#define WEB_TLS_CERT_SERIAL_BYTES 16U
#define WEB_TLS_CERT_SUBJECT "C=UA,O=GeoLocator,CN=geolocator.local"
/* Must be the Device CA's subject (tools/generate_geolocator_device_ca.py). */
#define WEB_TLS_CERT_ISSUER "C=UA,O=GeoLocator,CN=GeoLocator Device CA"
/* Backdated and long: the board has no clock when it boots, and the phone's clock decides, not ours. Inside the Device CA's life. */
#define WEB_TLS_CERT_NOT_BEFORE "20260101000000"
#define WEB_TLS_CERT_NOT_AFTER "20330101000000"

int web_tls_cert_parse_ipv4(const char *text, unsigned char octets[4])
{
    if (text == NULL || octets == NULL) return 0;
    unsigned char parsed[4];
    const char *cursor = text;
    for (unsigned part = 0U; part < 4U; ++part) {
        if (*cursor < '0' || *cursor > '9') return 0;
        unsigned value = 0U;
        unsigned digits = 0U;
        while (*cursor >= '0' && *cursor <= '9') {
            value = value * 10U + (unsigned)(*cursor - '0');
            if (value > 255U || ++digits > 3U) return 0;
            ++cursor;
        }
        parsed[part] = (unsigned char)value;
        if (part < 3U) {
            if (*cursor != '.') return 0;
            ++cursor;
        }
    }
    if (*cursor != '\0') return 0;
    memcpy(octets, parsed, sizeof(parsed));
    return 1;
}

static int web_tls_cert_parse_key(mbedtls_pk_context *key, const char *pem, const web_tls_cert_request_t *request)
{
    return mbedtls_pk_parse_key(
        key,
        (const unsigned char *)pem,
        strlen(pem) + 1U,
        NULL,
        0U,
        request->rng,
        request->rng_context
    );
}

int web_tls_cert_issue(const web_tls_cert_request_t *request, char *out, size_t out_size, size_t *out_length)
{
    if (request == NULL || out == NULL || out_size == 0U || request->leaf_key_pem == NULL || request->issuer_cert_pem == NULL ||
        request->issuer_key_pem == NULL || request->rng == NULL) {
        return -1;
    }
    if (request->ipv4_count > 0U && request->ipv4_addresses == NULL) return -1;

    unsigned char addresses[WEB_TLS_CERT_MAX_SAN_IPS][4];
    size_t address_count = 0U;
    for (size_t i = 0U; i < request->ipv4_count && address_count < WEB_TLS_CERT_MAX_SAN_IPS; ++i) {
        const char *text = request->ipv4_addresses[i];
        if (text == NULL || text[0] == '\0') continue;
        unsigned char octets[4];
        if (web_tls_cert_parse_ipv4(text, octets) == 0) continue;
        int duplicate = 0;
        for (size_t j = 0U; j < address_count; ++j) {
            if (memcmp(addresses[j], octets, sizeof(octets)) == 0) duplicate = 1;
        }
        if (duplicate == 0) {
            memcpy(addresses[address_count], octets, sizeof(octets));
            ++address_count;
        }
    }
    if (address_count == 0U) return -3;

    mbedtls_pk_context leaf_key;
    mbedtls_pk_context issuer_key;
    mbedtls_x509_crt issuer_cert;
    mbedtls_x509write_cert certificate;
    mbedtls_pk_init(&leaf_key);
    mbedtls_pk_init(&issuer_key);
    mbedtls_x509_crt_init(&issuer_cert);
    mbedtls_x509write_crt_init(&certificate);

    int result = web_tls_cert_parse_key(&leaf_key, request->leaf_key_pem, request);
    if (result == 0) result = web_tls_cert_parse_key(&issuer_key, request->issuer_key_pem, request);
    if (result == 0) {
        result = mbedtls_x509_crt_parse(&issuer_cert, (const unsigned char *)request->issuer_cert_pem, strlen(request->issuer_cert_pem) + 1U);
    }
    /* A Device CA certificate that does not belong to its key would sign a leaf no phone accepts: say so here. */
    if (result == 0) result = mbedtls_pk_check_pair(&issuer_cert.pk, &issuer_key, request->rng, request->rng_context);

    mbedtls_x509_san_list san_nodes[1U + WEB_TLS_CERT_MAX_SAN_IPS];
    memset(san_nodes, 0, sizeof(san_nodes));
    san_nodes[0].node.type = MBEDTLS_X509_SAN_DNS_NAME;
    san_nodes[0].node.san.unstructured_name.p = (unsigned char *)(uintptr_t)WEB_TLS_CERT_DNS_NAME;
    san_nodes[0].node.san.unstructured_name.len = sizeof(WEB_TLS_CERT_DNS_NAME) - 1U;
    for (size_t i = 0U; i < address_count; ++i) {
        san_nodes[1U + i].node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
        san_nodes[1U + i].node.san.unstructured_name.p = addresses[i];
        san_nodes[1U + i].node.san.unstructured_name.len = 4U;
    }
    for (size_t i = 0U; i < address_count; ++i) san_nodes[i].next = &san_nodes[i + 1U];
    san_nodes[address_count].next = NULL;

    unsigned char serial[WEB_TLS_CERT_SERIAL_BYTES];
    if (result == 0) {
        result = request->rng(request->rng_context, serial, sizeof(serial));
        serial[0] = (unsigned char)((serial[0] & 0x7FU) | 0x01U); /* positive, never zero */
    }

    mbedtls_asn1_sequence server_auth = {
        .buf = { .tag = MBEDTLS_ASN1_OID, .len = MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH), .p = (unsigned char *)MBEDTLS_OID_SERVER_AUTH },
        .next = NULL,
    };

    if (result == 0) {
        mbedtls_x509write_crt_set_version(&certificate, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&certificate, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&certificate, &leaf_key);
        mbedtls_x509write_crt_set_issuer_key(&certificate, &issuer_key);
        result = mbedtls_x509write_crt_set_serial_raw(&certificate, serial, sizeof(serial));
    }
    if (result == 0) result = mbedtls_x509write_crt_set_subject_name(&certificate, WEB_TLS_CERT_SUBJECT);
    if (result == 0) result = mbedtls_x509write_crt_set_issuer_name(&certificate, WEB_TLS_CERT_ISSUER);
    if (result == 0) result = mbedtls_x509write_crt_set_validity(&certificate, WEB_TLS_CERT_NOT_BEFORE, WEB_TLS_CERT_NOT_AFTER);
    if (result == 0) result = mbedtls_x509write_crt_set_basic_constraints(&certificate, 0, -1);
    if (result == 0) result = mbedtls_x509write_crt_set_subject_key_identifier(&certificate);
    if (result == 0) result = mbedtls_x509write_crt_set_authority_key_identifier(&certificate);
    if (result == 0) {
        result = mbedtls_x509write_crt_set_key_usage(&certificate, MBEDTLS_X509_KU_DIGITAL_SIGNATURE | MBEDTLS_X509_KU_KEY_ENCIPHERMENT);
    }
    if (result == 0) result = mbedtls_x509write_crt_set_ext_key_usage(&certificate, &server_auth);
    if (result == 0) result = mbedtls_x509write_crt_set_subject_alternative_name(&certificate, &san_nodes[0]);

    size_t leaf_length = 0U;
    if (result == 0) {
        result = mbedtls_x509write_crt_pem(&certificate, (unsigned char *)out, out_size, request->rng, request->rng_context);
        if (result == 0) leaf_length = strlen(out);
    }
    if (result == 0) {
        const size_t issuer_length = strlen(request->issuer_cert_pem);
        if (leaf_length + issuer_length + 1U > out_size) {
            result = -2;
        } else {
            memcpy(out + leaf_length, request->issuer_cert_pem, issuer_length + 1U);
            if (out_length != NULL) *out_length = leaf_length + issuer_length + 1U;
        }
    }

    mbedtls_x509write_crt_free(&certificate);
    mbedtls_x509_crt_free(&issuer_cert);
    mbedtls_pk_free(&issuer_key);
    mbedtls_pk_free(&leaf_key);
    return result;
}
