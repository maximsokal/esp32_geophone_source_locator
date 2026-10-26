/*
 * HTTP server input: percent-decoding of every byte value and the Wi-Fi setup
 * form at its size limits and with hostile fields.
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "web_server_internal.h"

#define CASE_PASS(name) printf("PERIPHERAL_CASE %s PASS\n", name)

static void percent_decoding_covers_every_byte_value(void)
{
    char output[8];
    static const char *const digits = "0123456789abcdefABCDEF";

    for (unsigned value = 0U; value <= 255U; ++value) {
        char source[4];
        (void)snprintf(source, sizeof(source), "%%%02X", value);
        const esp_err_t result = web_wifi_setup_url_decode(source, output, sizeof(output));
        if (value == 0U) {
            /* A decoded NUL would silently cut the SSID/password short. */
            assert(result == ESP_ERR_INVALID_ARG);
        } else {
            assert(result == ESP_OK);
            assert((unsigned char)output[0] == value && output[1] == '\0');
        }
    }
    /* Hex digits are case-insensitive, in every position. */
    for (size_t high = 0U; high < strlen(digits); ++high) {
        for (size_t low = 0U; low < strlen(digits); ++low) {
            char source[4] = {'%', digits[high], digits[low], '\0'};
            if (source[1] == '0' && source[2] == '0') continue;
            assert(web_wifi_setup_url_decode(source, output, sizeof(output)) == ESP_OK);
        }
    }
    /* Every non-hex character is rejected as a digit. */
    for (unsigned c = 1U; c <= 255U; ++c) {
        const bool is_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        char source[4] = {'%', (char)c, '0', '\0'};
        char second[4] = {'%', '0', (char)c, '\0'};
        if (c == '0') continue;
        assert((web_wifi_setup_url_decode(source, output, sizeof(output)) == ESP_OK) == is_hex);
        assert((web_wifi_setup_url_decode(second, output, sizeof(output)) == ESP_OK) == is_hex);
    }
    assert(web_wifi_setup_url_decode("a%2", output, sizeof(output)) == ESP_ERR_INVALID_ARG);
    assert(web_wifi_setup_url_decode("a%", output, sizeof(output)) == ESP_ERR_INVALID_ARG);
    assert(web_wifi_setup_url_decode("%%41", output, sizeof(output)) == ESP_ERR_INVALID_ARG);
    CASE_PASS("HTTP.percent_decoding");
}

static void decoded_output_always_fits_or_fails_cleanly(void)
{
    char output[32];
    const char *const source = "a+b%20c%2B%7e";
    const size_t decoded_length = strlen("a b c+~");

    for (size_t size = 1U; size < sizeof(output); ++size) {
        memset(output, 'x', sizeof(output));
        const esp_err_t result = web_wifi_setup_url_decode(source, output, size);
        if (size >= decoded_length + 1U) {
            assert(result == ESP_OK && strcmp(output, "a b c+~") == 0);
        } else {
            assert(result == ESP_ERR_INVALID_SIZE);
        }
        /* Nothing is ever written past the destination. */
        for (size_t i = size; i < sizeof(output); ++i) assert(output[i] == 'x');
    }
    assert(web_wifi_setup_url_decode("", output, 1U) == ESP_OK && output[0] == '\0');
    CASE_PASS("HTTP.decode_size_boundary");
}

static size_t encode_all(const char *text, char *out, size_t out_size)
{
    size_t length = 0U;
    for (const unsigned char *c = (const unsigned char *)text; *c != '\0'; ++c) {
        assert(length + 3U < out_size);
        (void)snprintf(out + length, out_size - length, "%%%02X", *c);
        length += 3U;
    }
    return length;
}

static void setup_form_accepts_the_largest_legal_credentials(void)
{
    char ssid_text[33];
    char password_text[64];
    char encoded_ssid[128];
    char encoded_password[256];
    char body[512];
    char ssid[33];
    char password[64];

    /* 32-byte SSID and 63-byte WPA2 password, every byte percent-encoded: the worst case. */
    memset(ssid_text, 'S', 32U);
    ssid_text[32] = '\0';
    memset(password_text, '!', 63U);
    password_text[63] = '\0';
    (void)encode_all(ssid_text, encoded_ssid, sizeof(encoded_ssid));
    (void)encode_all(password_text, encoded_password, sizeof(encoded_password));
    const int written = snprintf(body, sizeof(body), "ssid=%s&password=%s", encoded_ssid, encoded_password);
    assert(written == 5 + 96 + 10 + 189);

    assert(web_wifi_setup_form_parse(body, ssid, sizeof(ssid), password, sizeof(password)) == ESP_OK);
    assert(strcmp(ssid, ssid_text) == 0);
    assert(strcmp(password, password_text) == 0);
    CASE_PASS("HTTP.setup_form_worst_case_length");
}

static void setup_form_field_selection_and_errors(void)
{
    char ssid[33];
    char password[64];
    char body[256];

    (void)snprintf(body, sizeof(body), "ssid=Home&password=secret");
    assert(web_wifi_setup_form_parse(body, ssid, sizeof(ssid), password, sizeof(password)) == ESP_OK);
    assert(strcmp(ssid, "Home") == 0 && strcmp(password, "secret") == 0);

    /* Field order, unrelated keys and key prefixes. */
    (void)snprintf(body, sizeof(body), "x=1&password=pw&xssid=bad&ssid=Real&pass=nope");
    assert(web_wifi_setup_form_parse(body, ssid, sizeof(ssid), password, sizeof(password)) == ESP_OK);
    assert(strcmp(ssid, "Real") == 0 && strcmp(password, "pw") == 0);

    /* The first occurrence of a key wins. */
    (void)snprintf(body, sizeof(body), "ssid=First&ssid=Second&password=a&password=b");
    assert(web_wifi_setup_form_parse(body, ssid, sizeof(ssid), password, sizeof(password)) == ESP_OK);
    assert(strcmp(ssid, "First") == 0 && strcmp(password, "a") == 0);

    /* A missing password means an open network; a missing SSID is an error. */
    (void)snprintf(body, sizeof(body), "ssid=Open");
    memset(password, 'x', sizeof(password));
    assert(web_wifi_setup_form_parse(body, ssid, sizeof(ssid), password, sizeof(password)) == ESP_OK);
    assert(password[0] == '\0');
    (void)snprintf(body, sizeof(body), "password=only");
    assert(web_wifi_setup_form_parse(body, ssid, sizeof(ssid), password, sizeof(password)) == ESP_ERR_INVALID_ARG);
    (void)snprintf(body, sizeof(body), "");
    assert(web_wifi_setup_form_parse(body, ssid, sizeof(ssid), password, sizeof(password)) == ESP_ERR_INVALID_ARG);

    /* A destination that is too small is reported, never overrun. */
    (void)snprintf(body, sizeof(body), "ssid=%s&password=x", "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefgh");
    assert(web_wifi_setup_form_parse(body, ssid, sizeof(ssid), password, sizeof(password)) == ESP_ERR_INVALID_SIZE);

    /* A NUL smuggled through encoding cannot truncate the password. */
    (void)snprintf(body, sizeof(body), "ssid=Home&password=pass%%00word");
    assert(web_wifi_setup_form_parse(body, ssid, sizeof(ssid), password, sizeof(password)) == ESP_ERR_INVALID_ARG);

    assert(web_wifi_setup_form_parse(NULL, ssid, sizeof(ssid), password, sizeof(password)) == ESP_ERR_INVALID_ARG);
    (void)snprintf(body, sizeof(body), "ssid=a");
    assert(web_wifi_setup_form_parse(body, NULL, sizeof(ssid), password, sizeof(password)) == ESP_ERR_INVALID_ARG);
    assert(web_wifi_setup_form_parse(body, ssid, sizeof(ssid), NULL, sizeof(password)) == ESP_ERR_INVALID_ARG);
    CASE_PASS("HTTP.setup_form_fields");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    percent_decoding_covers_every_byte_value();
    decoded_output_always_fits_or_fails_cleanly();
    setup_form_field_selection_and_errors();
    setup_form_accepts_the_largest_legal_credentials();
    return 0;
}
