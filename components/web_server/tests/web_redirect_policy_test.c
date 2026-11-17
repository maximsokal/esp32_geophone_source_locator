#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "web_redirect_policy.h"

#define FALLBACK "https://geolocator.local/"

static const char *redirect(const char *host, bool *kept)
{
    static char out[128];
    const bool result = web_redirect_https_location(host, FALLBACK, out, sizeof(out));
    if (kept != NULL) *kept = result;
    return out;
}

/*
 * Board 2026-11-17: the phone could not open https://geolocator.local/ (no ".local" resolution), typed the device's address
 * and was sent to the very name it cannot resolve. The redirect keeps the host the client used.
 */
static void test_the_host_the_client_used_is_kept(void)
{
    bool kept = false;
    assert(strcmp(redirect("192.168.0.191", &kept), "https://192.168.0.191/") == 0 && kept);
    assert(strcmp(redirect("192.168.0.191:80", &kept), "https://192.168.0.191/") == 0 && kept);   /* the port is HTTP's, not HTTPS's */
    assert(strcmp(redirect("geolocator.local", &kept), "https://geolocator.local/") == 0 && kept);
    assert(strcmp(redirect("GeoLocator.LOCAL:80", &kept), "https://GeoLocator.LOCAL/") == 0 && kept);
    assert(strcmp(redirect("  192.168.4.1  ", &kept), "https://192.168.4.1/") == 0 && kept);
    assert(strcmp(redirect("my-device.example", &kept), "https://my-device.example/") == 0 && kept);
}

static void test_a_host_that_cannot_be_trusted_gives_the_name_that_always_worked_before(void)
{
    bool kept = true;
    assert(strcmp(redirect(NULL, &kept), FALLBACK) == 0 && !kept);
    assert(strcmp(redirect("", &kept), FALLBACK) == 0 && !kept);
    assert(strcmp(redirect(":80", &kept), FALLBACK) == 0 && !kept);
    assert(strcmp(redirect("[fe80::1]:80", &kept), FALLBACK) == 0 && !kept);                  /* an IPv6 literal */
    assert(strcmp(redirect("evil.example\r\nSet-Cookie: x=1", &kept), FALLBACK) == 0 && !kept); /* header injection */
    assert(strcmp(redirect("a b", &kept), FALLBACK) == 0 && !kept);
    assert(strcmp(redirect("host/path", &kept), FALLBACK) == 0 && !kept);
    assert(strcmp(redirect("host@other", &kept), FALLBACK) == 0 && !kept);
    assert(strcmp(redirect("a..b", &kept), FALLBACK) == 0 && !kept);
    assert(strcmp(redirect(".local", &kept), FALLBACK) == 0 && !kept);
    assert(strcmp(redirect("192.168.0.191.", &kept), FALLBACK) == 0 && !kept);
    assert(strcmp(redirect("host:80x", &kept), FALLBACK) == 0 && !kept);
    assert(strcmp(redirect("host:", &kept), FALLBACK) == 0 && !kept);
    assert(strcmp(redirect("host:80 junk", &kept), FALLBACK) == 0 && !kept);
}

static void test_length_limits_and_small_buffers(void)
{
    char host[80];
    memset(host, 'a', 63U);
    host[63] = '\0';
    bool kept = false;
    char expected[80];
    (void)snprintf(expected, sizeof(expected), "https://%s/", host);
    assert(strcmp(redirect(host, &kept), expected) == 0 && kept);
    host[63] = 'a';
    host[64] = '\0';
    assert(strcmp(redirect(host, &kept), FALLBACK) == 0 && !kept);                              /* 64 characters */

    char tiny[16];
    assert(!web_redirect_https_location("192.168.0.191", FALLBACK, tiny, sizeof(tiny)));      /* does not fit: the fallback is cut */
    assert(strlen(tiny) < sizeof(tiny));
    assert(!web_redirect_https_location("192.168.0.191", FALLBACK, NULL, 8U));
    assert(!web_redirect_https_location("192.168.0.191", FALLBACK, tiny, 0U));
    char one[1];
    assert(web_redirect_https_location("a", FALLBACK, one, sizeof(one)) == false && one[0] == '\0');
    char fallback_only[32];
    assert(!web_redirect_https_location(NULL, NULL, fallback_only, sizeof(fallback_only)) && strcmp(fallback_only, "/") == 0);
}

int main(void)
{
    test_the_host_the_client_used_is_kept();
    test_a_host_that_cannot_be_trusted_gives_the_name_that_always_worked_before();
    test_length_limits_and_small_buffers();
    puts("web_redirect_policy_test: OK");
    return 0;
}
