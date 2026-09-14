#include <assert.h>
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

int main(void)
{
    test_address_policy();
    test_write_forwards_exact_transfer();
    test_write_read_forwards_exact_transfer();
    test_invalid_arguments_do_not_touch_transport();
    test_transport_failure_is_normalized();
    return 0;
}
