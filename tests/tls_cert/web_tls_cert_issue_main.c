/*
 * Host driver for web_tls_cert_issue: reads the leaf key, the Device CA certificate and key, issues the certificate for the
 * addresses given on the command line and writes the chain to a file. tests/python/test_web_tls_cert.py checks it with openssl.
 *
 *   web_tls_cert_issue_main <leaf.key> <deviceCA.crt> <deviceCA.key> <out.pem> [ip ...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/x509_crt.h"

#include "web_tls_cert.h"

static char *slurp(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *text = malloc((size_t)size + 1U);
    if (text == NULL) { fclose(file); return NULL; }
    size_t got = fread(text, 1U, (size_t)size, file);
    text[got] = '\0';
    fclose(file);
    return text;
}

/* A test-only random source: the serial numbers differ run to run, nothing here is secret. */
static unsigned long long s_state = 0x9E3779B97F4A7C15ULL;
static int test_rng(void *context, unsigned char *buffer, size_t length)
{
    (void)context;
    for (size_t i = 0U; i < length; ++i) {
        s_state = s_state * 6364136223846793005ULL + 1442695040888963407ULL;
        buffer[i] = (unsigned char)(s_state >> 56);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 5) return 64;
    char *leaf_key = slurp(argv[1]);
    char *issuer_cert = slurp(argv[2]);
    char *issuer_key = slurp(argv[3]);
    if (leaf_key == NULL || issuer_cert == NULL || issuer_key == NULL) return 65;

    const char *addresses[8];
    size_t count = 0U;
    for (int i = 5; i < argc && count < 8U; ++i) addresses[count++] = argv[i];

    const web_tls_cert_request_t request = {
        .leaf_key_pem = leaf_key, .issuer_cert_pem = issuer_cert, .issuer_key_pem = issuer_key,
        .ipv4_addresses = addresses, .ipv4_count = count, .rng = test_rng, .rng_context = NULL,
    };
    static char out[4096];
    size_t length = 0U;
    int result = web_tls_cert_issue(&request, out, sizeof(out), &length);
    if (result != 0) { fprintf(stderr, "issue failed: %d\n", result); return 1; }
    if (strlen(out) + 1U != length) { fprintf(stderr, "length mismatch\n"); return 2; }
    /* The HTTPS server hands this text to mbedtls_x509_crt_parse; it must accept the whole chain (leaf then Device CA). */
    mbedtls_x509_crt parsed;
    mbedtls_x509_crt_init(&parsed);
    int parse = mbedtls_x509_crt_parse(&parsed, (const unsigned char *)out, length);
    int certificates = 0;
    for (const mbedtls_x509_crt *c = &parsed; c != NULL && c->raw.p != NULL; c = c->next) ++certificates;
    mbedtls_x509_crt_free(&parsed);
    if (parse != 0 || certificates != 2) { fprintf(stderr, "chain parse %d, %d certificates\n", parse, certificates); return 3; }
    FILE *file = fopen(argv[4], "wb");
    if (file == NULL) return 66;
    fwrite(out, 1U, strlen(out), file);
    fclose(file);
    printf("OK %u\n", (unsigned)length);
    return 0;
}
