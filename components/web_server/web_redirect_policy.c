#include "web_redirect_policy.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define WEB_REDIRECT_HOST_MAX 63U

static bool host_character(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
}

static void use_fallback(const char *fallback_url, char *out, size_t out_size)
{
    if (out != NULL && out_size > 0U) {
        (void)snprintf(out, out_size, "%s", fallback_url != NULL ? fallback_url : "/");
    }
}

bool web_redirect_https_location(const char *host_header, const char *fallback_url, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0U) return false;
    use_fallback(fallback_url, out, out_size);
    if (host_header == NULL) return false;

    /* Skip leading blanks, cut at the port or at trailing blanks. */
    while (*host_header == ' ' || *host_header == '\t') ++host_header;
    char host[WEB_REDIRECT_HOST_MAX + 1U];
    size_t length = 0U;
    for (; host_header[length] != '\0' && host_header[length] != ':' && host_header[length] != ' ' && host_header[length] != '\t';
         ++length) {
        if (length >= WEB_REDIRECT_HOST_MAX || !host_character(host_header[length])) return false;
        host[length] = host_header[length];
    }
    host[length] = '\0';
    if (length == 0U) return false;

    /* After the host: nothing, or a numeric port, or blanks. Anything else is not a Host header we understand. */
    const char *rest = host_header + length;
    while (*rest == ' ' || *rest == '\t') ++rest;
    if (*rest == ':') {
        ++rest;
        if (*rest == '\0') return false;
        for (; *rest >= '0' && *rest <= '9'; ++rest) {
        }
        while (*rest == ' ' || *rest == '\t') ++rest;
    }
    if (*rest != '\0') return false;

    /* No empty label ("a..b", ".a", "a."): not a name or an address. */
    if (host[0] == '.' || host[length - 1U] == '.' || strstr(host, "..") != NULL) return false;

    const int written = snprintf(out, out_size, "https://%s/", host);
    if (written < 0 || (size_t)written >= out_size) {
        use_fallback(fallback_url, out, out_size);
        return false;
    }
    return true;
}
