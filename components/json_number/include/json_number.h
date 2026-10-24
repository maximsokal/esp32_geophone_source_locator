#pragma once

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * One contract for turning a JSON number (always a double in cJSON) into an
 * integer type: finite, integral, and inside the target range, checked BEFORE
 * any conversion. A bare (T)double cast of an out-of-range or non-finite value
 * is undefined behaviour in C, and a "never negative" test does not prevent it.
 *
 * On failure *out is left untouched. Header-only and free of libm calls so
 * every host test target and the firmware can use it unchanged.
 */

#define JSON_NUMBER_TWO_POW_64 18446744073709551616.0

static inline bool json_number_to_uint64(double value, uint64_t *out)
{
    if (
        out == NULL ||
        !isfinite(value) ||
        value < 0.0 ||
        value >= JSON_NUMBER_TWO_POW_64
    ) {
        return false;
    }
    const uint64_t converted = (uint64_t)value;
    if ((double)converted != value) {
        return false;
    }
    *out = converted;
    return true;
}

static inline bool json_number_to_uint32(double value, uint32_t *out)
{
    uint64_t wide = 0U;
    if (out == NULL || !json_number_to_uint64(value, &wide) || wide > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)wide;
    return true;
}

static inline bool json_number_to_size(double value, size_t *out)
{
    uint64_t wide = 0U;
    if (out == NULL || !json_number_to_uint64(value, &wide) || wide > (uint64_t)SIZE_MAX) {
        return false;
    }
    *out = (size_t)wide;
    return true;
}

static inline bool json_number_to_int64(double value, int64_t *out)
{
    /* [-2^63, 2^63): both bounds are exactly representable as doubles. */
    if (
        out == NULL ||
        !isfinite(value) ||
        value < -9223372036854775808.0 ||
        value >= 9223372036854775808.0
    ) {
        return false;
    }
    const int64_t converted = (int64_t)value;
    if ((double)converted != value) {
        return false;
    }
    *out = converted;
    return true;
}

static inline bool json_number_to_int32(double value, int32_t *out)
{
    if (
        out == NULL ||
        !isfinite(value) ||
        value < -2147483648.0 ||
        value > 2147483647.0
    ) {
        return false;
    }
    const int32_t converted = (int32_t)value;
    if ((double)converted != value) {
        return false;
    }
    *out = converted;
    return true;
}
