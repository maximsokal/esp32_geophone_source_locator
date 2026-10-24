#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "json_number.h"

/* The out-parameter must be left untouched whenever a value is rejected. */
#define SENTINEL_U64 UINT64_C(0xA5A5A5A5A5A5A5A5)

static void test_uint64_contract(void)
{
    uint64_t out = SENTINEL_U64;
    assert(json_number_to_uint64(0.0, &out) && out == 0U);
    assert(json_number_to_uint64(-0.0, &out) && out == 0U);
    assert(json_number_to_uint64(1.0, &out) && out == 1U);
    assert(json_number_to_uint64(9007199254740992.0, &out) && out == UINT64_C(9007199254740992));
    /* Largest double below 2^64. */
    assert(json_number_to_uint64(18446744073709549568.0, &out));
    assert(out == UINT64_C(18446744073709549568));

    const double rejected[] = {
        -1.0, -0.5, 0.5, 1.5, 1e30, 1e308, -1e30,
        18446744073709551616.0, 18446744073709551615.0, /* rounds to 2^64 */
        INFINITY, -INFINITY, NAN,
    };
    for (size_t i = 0U; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        out = SENTINEL_U64;
        assert(!json_number_to_uint64(rejected[i], &out));
        assert(out == SENTINEL_U64);
    }
    assert(!json_number_to_uint64(1.0, NULL));
}

static void test_uint32_and_size_contract(void)
{
    uint32_t out32 = 0xDEADBEEFU;
    assert(json_number_to_uint32(4294967295.0, &out32) && out32 == UINT32_MAX);
    out32 = 0xDEADBEEFU;
    assert(!json_number_to_uint32(4294967296.0, &out32) && out32 == 0xDEADBEEFU);
    assert(!json_number_to_uint32(-1.0, &out32));
    assert(!json_number_to_uint32(2.5, &out32));
    assert(!json_number_to_uint32(NAN, &out32));

    size_t size_out = 77U;
    assert(json_number_to_size(65536.0, &size_out) && size_out == 65536U);
    size_out = 77U;
    assert(!json_number_to_size(1e30, &size_out) && size_out == 77U);
    assert(!json_number_to_size(1.5, &size_out) && size_out == 77U);
    assert(!json_number_to_size(-1.0, &size_out) && size_out == 77U);
    assert(!json_number_to_size(INFINITY, &size_out) && size_out == 77U);
    assert(!json_number_to_size(18446744073709551616.0, &size_out) && size_out == 77U);
    assert(!json_number_to_size(1.0, NULL));
}

static void test_int32_contract(void)
{
    int32_t out = 12345;
    assert(json_number_to_int32(0.0, &out) && out == 0);
    assert(json_number_to_int32(-0.0, &out) && out == 0);
    assert(json_number_to_int32(-2147483648.0, &out) && out == INT32_MIN);
    assert(json_number_to_int32(2147483647.0, &out) && out == INT32_MAX);
    assert(json_number_to_int32(-1250.0, &out) && out == -1250);

    const double rejected[] = {
        -2147483649.0, 2147483648.0, 1e30, -1e30, 0.5, -0.5, 1250.25,
        INFINITY, -INFINITY, NAN,
    };
    for (size_t i = 0U; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        out = 12345;
        assert(!json_number_to_int32(rejected[i], &out));
        assert(out == 12345);
    }
    assert(!json_number_to_int32(1.0, NULL));
}

static void test_int64_contract(void)
{
    int64_t out = 99;
    assert(json_number_to_int64(0.0, &out) && out == 0);
    assert(json_number_to_int64(-9223372036854775808.0, &out) && out == INT64_MIN);
    assert(json_number_to_int64(9223372036854774784.0, &out)); /* largest double < 2^63 */
    assert(out == INT64_C(9223372036854774784));
    assert(json_number_to_int64(-1234567.0, &out) && out == -1234567);

    const double rejected[] = {
        9223372036854775808.0, -9223372036854777856.0, 1e30, -1e30, 0.5, -2.25,
        INFINITY, -INFINITY, NAN,
    };
    for (size_t i = 0U; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        out = 99;
        assert(!json_number_to_int64(rejected[i], &out));
        assert(out == 99);
    }
    assert(!json_number_to_int64(1.0, NULL));
}

int main(void)
{
    test_int64_contract();
    test_uint64_contract();
    test_uint32_and_size_contract();
    test_int32_contract();
    return 0;
}
