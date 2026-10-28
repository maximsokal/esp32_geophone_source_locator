#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "nrf_sensor_bus.h"

typedef struct {
    int return_code;
    int calls;
    uint8_t address;
    const uint8_t *write_data;
    size_t write_len;
    uint8_t *read_data;
    size_t read_len;
} fake_bus_context_t;

static int fake_transfer(
    void *context,
    uint8_t address,
    const uint8_t *write_data,
    size_t write_len,
    uint8_t *read_data,
    size_t read_len)
{
    fake_bus_context_t *fake = (fake_bus_context_t *)context;
    fake->calls++;
    fake->address = address;
    fake->write_data = write_data;
    fake->write_len = write_len;
    fake->read_data = read_data;
    fake->read_len = read_len;

    if (read_data != NULL && read_len > 0U) {
        memset(read_data, 0xA5, read_len);
    }
    return fake->return_code;
}

static void test_address_policy(void)
{
    assert(!nrf_sensor_bus_is_valid_address(0x00U));
    assert(!nrf_sensor_bus_is_valid_address(0x07U));
    assert(nrf_sensor_bus_is_valid_address(0x08U));
    assert(nrf_sensor_bus_is_valid_address(0x68U));
    assert(nrf_sensor_bus_is_valid_address(0x76U));
    assert(nrf_sensor_bus_is_valid_address(0x77U));
    assert(!nrf_sensor_bus_is_valid_address(0x78U));
    assert(!nrf_sensor_bus_is_valid_address(0xFFU));
}

static void test_write_forwards_exact_transfer(void)
{
    fake_bus_context_t fake = {0};
    const nrf_sensor_bus_t bus = {
        .transfer = fake_transfer,
        .context = &fake,
    };
    const uint8_t payload[] = {0x7EU, 0x11U};

    assert(nrf_sensor_bus_write(&bus, 0x68U, payload, sizeof(payload)) ==
           NRF_SENSOR_BUS_OK);
    assert(fake.calls == 1);
    assert(fake.address == 0x68U);
    assert(fake.write_data == payload);
    assert(fake.write_len == sizeof(payload));
    assert(fake.read_data == NULL);
    assert(fake.read_len == 0U);
}

static void test_write_read_forwards_exact_transfer(void)
{
    fake_bus_context_t fake = {0};
    const nrf_sensor_bus_t bus = {
        .transfer = fake_transfer,
        .context = &fake,
    };
    const uint8_t reg = 0xD0U;
    uint8_t result[2] = {0};

    assert(nrf_sensor_bus_write_read(
               &bus,
               0x76U,
               &reg,
               sizeof(reg),
               result,
               sizeof(result)) == NRF_SENSOR_BUS_OK);
    assert(fake.calls == 1);
    assert(fake.address == 0x76U);
    assert(fake.write_data == &reg);
    assert(fake.write_len == sizeof(reg));
    assert(fake.read_data == result);
    assert(fake.read_len == sizeof(result));
    assert(result[0] == 0xA5U && result[1] == 0xA5U);
}

static void test_invalid_arguments_do_not_touch_transport(void)
{
    fake_bus_context_t fake = {0};
    const nrf_sensor_bus_t bus = {
        .transfer = fake_transfer,
        .context = &fake,
    };
    const uint8_t byte = 0U;
    uint8_t out = 0U;

    assert(nrf_sensor_bus_write(NULL, 0x68U, &byte, 1U) ==
           NRF_SENSOR_BUS_ERR_ARGUMENT);
    assert(nrf_sensor_bus_write(&bus, 0x00U, &byte, 1U) ==
           NRF_SENSOR_BUS_ERR_ADDRESS);
    assert(nrf_sensor_bus_write(&bus, 0x68U, NULL, 1U) ==
           NRF_SENSOR_BUS_ERR_ARGUMENT);
    assert(nrf_sensor_bus_write(&bus, 0x68U, &byte, 0U) ==
           NRF_SENSOR_BUS_ERR_ARGUMENT);
    assert(nrf_sensor_bus_write_read(&bus, 0x68U, NULL, 1U, &out, 1U) ==
           NRF_SENSOR_BUS_ERR_ARGUMENT);
    assert(nrf_sensor_bus_write_read(&bus, 0x68U, &byte, 1U, NULL, 1U) ==
           NRF_SENSOR_BUS_ERR_ARGUMENT);
    assert(fake.calls == 0);
}

static void test_transport_failure_is_normalized(void)
{
    fake_bus_context_t fake = {.return_code = -17};
    const nrf_sensor_bus_t bus = {
        .transfer = fake_transfer,
        .context = &fake,
    };
    const uint8_t byte = 0U;

    assert(nrf_sensor_bus_write(&bus, 0x68U, &byte, 1U) ==
           NRF_SENSOR_BUS_ERR_TRANSFER);
    assert(fake.calls == 1);
}

static void test_transport_failures_keep_their_cause(void)
{
    static const struct {
        int result;
        nrf_sensor_bus_result_t expected;
    } cases[] = {
        {0, NRF_SENSOR_BUS_OK},
        {-ETIMEDOUT, NRF_SENSOR_BUS_ERR_TIMEOUT},
        {-EIO, NRF_SENSOR_BUS_ERR_IO},
        {-EFAULT, NRF_SENSOR_BUS_ERR_IO},
        {-EAGAIN, NRF_SENSOR_BUS_ERR_IO},
        {-EBUSY, NRF_SENSOR_BUS_ERR_TRANSFER},
        {-ENODEV, NRF_SENSOR_BUS_ERR_TRANSFER},
        {-ENXIO, NRF_SENSOR_BUS_ERR_TRANSFER},
        {1, NRF_SENSOR_BUS_ERR_TRANSFER},
    };
    const uint8_t payload[2] = {0x7EU, 0x11U};
    uint8_t out = 0U;

    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        fake_bus_context_t fake = {.return_code = cases[i].result};
        const nrf_sensor_bus_t bus = {.transfer = fake_transfer, .context = &fake};
        assert(nrf_sensor_bus_classify(cases[i].result) == cases[i].expected);
        assert(nrf_sensor_bus_write(&bus, 0x68U, payload, sizeof(payload)) == cases[i].expected);
        assert(nrf_sensor_bus_write_read(&bus, 0x76U, payload, 1U, &out, 1U) == cases[i].expected);
        assert(fake.calls == 2);
    }
}

static nrf_sensor_bus_fault_t make_fault(
    uint8_t address,
    nrf_sensor_bus_result_t cause,
    bool lines_valid,
    bool sda_high,
    bool scl_high)
{
    return (nrf_sensor_bus_fault_t){
        .address = address,
        .cause = cause,
        .errno_abs = (uint8_t)EIO,
        .lines_valid = lines_valid,
        .sda_high = sda_high,
        .scl_high = scl_high,
        .failures = 1U,
    };
}

static void test_fault_pack_layout_and_roundtrip(void)
{
    nrf_sensor_bus_fault_t fault = make_fault(0x68U, NRF_SENSOR_BUS_ERR_IO, true, true, false);
    fault.errno_abs = 5U;
    fault.failures = 3U;
    /* address 0x68 | cause IO (2) << 8 | errno 5 << 16 | SDA high | levels valid | 3 failures << 27 */
    assert(nrf_sensor_bus_fault_pack(&fault) == (UINT32_C(0x68) | (UINT32_C(2) << 8) | (UINT32_C(5) << 16) |
                                                 (UINT32_C(1) << 24) | (UINT32_C(1) << 26) | (UINT32_C(3) << 27)));

    static const nrf_sensor_bus_result_t causes[] = {
        NRF_SENSOR_BUS_ERR_TIMEOUT, NRF_SENSOR_BUS_ERR_IO, NRF_SENSOR_BUS_ERR_TRANSFER,
    };
    static const uint8_t addresses[] = {0x00U, 0x08U, 0x77U, 0x7FU, 0xFFU};
    static const uint8_t errnos[] = {0U, 5U, 116U, 255U};
    for (size_t c = 0U; c < sizeof(causes) / sizeof(causes[0]); ++c) {
        for (size_t a = 0U; a < sizeof(addresses); ++a) {
            for (size_t e = 0U; e < sizeof(errnos); ++e) {
                for (unsigned lines = 0U; lines < 5U; ++lines) {
                    nrf_sensor_bus_fault_t in = make_fault(addresses[a], causes[c], lines != 0U,
                                                           (lines & 1U) != 0U, (lines & 2U) != 0U);
                    in.errno_abs = errnos[e];
                    in.failures = lines * 7U;
                    nrf_sensor_bus_fault_t out = {0};
                    assert(nrf_sensor_bus_fault_unpack(nrf_sensor_bus_fault_pack(&in), &out));
                    assert(out.address == in.address && out.cause == in.cause && out.errno_abs == in.errno_abs);
                    assert(out.lines_valid == in.lines_valid && out.sda_high == in.sda_high &&
                           out.scl_high == in.scl_high && out.failures == in.failures);
                }
            }
        }
    }
}

static void test_fault_pack_saturates_failures_and_hides_unsampled_levels(void)
{
    nrf_sensor_bus_fault_t fault = make_fault(0x76U, NRF_SENSOR_BUS_ERR_TIMEOUT, true, true, true);
    nrf_sensor_bus_fault_t out = {0};

    fault.failures = 31U;
    assert((nrf_sensor_bus_fault_pack(&fault) >> 27) == 31U);
    fault.failures = 32U;
    assert((nrf_sensor_bus_fault_pack(&fault) >> 27) == 31U);
    fault.failures = UINT32_MAX;
    assert((nrf_sensor_bus_fault_pack(&fault) >> 27) == 31U);

    /* Levels that were never sampled must not leak into the wire word as if they had been. */
    fault = make_fault(0x76U, NRF_SENSOR_BUS_ERR_IO, false, true, true);
    assert((nrf_sensor_bus_fault_pack(&fault) & (UINT32_C(7) << 24)) == 0U);
    assert(nrf_sensor_bus_fault_unpack(nrf_sensor_bus_fault_pack(&fault), &out));
    assert(!out.lines_valid && !out.sda_high && !out.scl_high);

    assert(nrf_sensor_bus_fault_pack(NULL) == 0U);
}

static void test_fault_unpack_rejects_what_pack_never_produces(void)
{
    nrf_sensor_bus_fault_t out = {0};
    assert(!nrf_sensor_bus_fault_unpack(0U, &out));                        /* cause 0 */
    assert(!nrf_sensor_bus_fault_unpack(UINT32_C(4) << 8, &out));          /* cause 4 */
    assert(!nrf_sensor_bus_fault_unpack(UINT32_C(0xFF) << 8, &out));
    assert(!nrf_sensor_bus_fault_unpack(UINT32_C(1) << 8, NULL));
    assert(nrf_sensor_bus_fault_unpack(UINT32_C(1) << 8, &out) && out.cause == NRF_SENSOR_BUS_ERR_TIMEOUT);
    assert(nrf_sensor_bus_fault_unpack(UINT32_C(3) << 8, &out) && out.cause == NRF_SENSOR_BUS_ERR_TRANSFER);
}

static void test_fault_rank_prefers_stuck_line_then_timeout_then_other_then_io(void)
{
    const nrf_sensor_bus_fault_t stuck = make_fault(0x77U, NRF_SENSOR_BUS_ERR_IO, true, false, true);
    const nrf_sensor_bus_fault_t timeout = make_fault(0x76U, NRF_SENSOR_BUS_ERR_TIMEOUT, true, true, true);
    const nrf_sensor_bus_fault_t other = make_fault(0x76U, NRF_SENSOR_BUS_ERR_TRANSFER, true, true, true);
    const nrf_sensor_bus_fault_t io = make_fault(0x76U, NRF_SENSOR_BUS_ERR_IO, true, true, true);
    const nrf_sensor_bus_fault_t unsampled_io = make_fault(0x76U, NRF_SENSOR_BUS_ERR_IO, false, false, false);

    assert(nrf_sensor_bus_fault_outranks(&stuck, &timeout));
    assert(!nrf_sensor_bus_fault_outranks(&timeout, &stuck));
    assert(nrf_sensor_bus_fault_outranks(&timeout, &other));
    assert(nrf_sensor_bus_fault_outranks(&other, &io));
    assert(!nrf_sensor_bus_fault_outranks(&io, &other));
    /* Unsampled levels are not evidence of a stuck line. */
    assert(!nrf_sensor_bus_fault_outranks(&unsampled_io, &io) && !nrf_sensor_bus_fault_outranks(&io, &unsampled_io));

    /* Equal rank: the lower address wins; the same address never displaces itself. */
    const nrf_sensor_bus_fault_t io_high = make_fault(0x77U, NRF_SENSOR_BUS_ERR_IO, true, true, true);
    assert(nrf_sensor_bus_fault_outranks(&io, &io_high));
    assert(!nrf_sensor_bus_fault_outranks(&io_high, &io));
    assert(!nrf_sensor_bus_fault_outranks(&io, &io));

    assert(nrf_sensor_bus_fault_outranks(&io, NULL));
    assert(!nrf_sensor_bus_fault_outranks(NULL, &io));
}

int main(void)
{
    test_transport_failures_keep_their_cause();
    test_fault_pack_layout_and_roundtrip();
    test_fault_pack_saturates_failures_and_hides_unsampled_levels();
    test_fault_unpack_rejects_what_pack_never_produces();
    test_fault_rank_prefers_stuck_line_then_timeout_then_other_then_io();
    test_address_policy();
    test_write_forwards_exact_transfer();
    test_write_read_forwards_exact_transfer();
    test_invalid_arguments_do_not_touch_transport();
    test_transport_failure_is_normalized();
    return 0;
}
