#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * Where plain HTTP sends a browser: the same host it asked for, over HTTPS.
 *
 * It used to send everyone to the mDNS name (https://geolocator.local/). A phone that cannot resolve ".local" (Android
 * often cannot) typed the device's address, was redirected to the name it cannot resolve, and could not connect (board
 * 2026-11-17). The host the client used is the host that works for it, so the redirect keeps it.
 *
 * `host_header` is the request's Host header, with or without a port. Only a plain host name or IPv4 address is kept: letters,
 * digits, dots and hyphens, at most 63 characters, no empty label. Anything else (a missing header, an IPv6 literal, control
 * characters, a header-injection attempt) gives the fallback. Returns true when the host of the request was kept.
 */
bool web_redirect_https_location(const char *host_header, const char *fallback_url, char *out, size_t out_size);
