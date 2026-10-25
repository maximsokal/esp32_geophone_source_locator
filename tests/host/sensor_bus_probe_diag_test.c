#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "sensor_bus_probe_diag.h"

static int64_t s_now_us = 1000;
static bool s_owner_held;
static bool s_after_transfer;
static esp_err_t s_transfer_result;
static unsigned s_lines;
static unsigned s_overflow;
static char s_last_line[512];

int64_t esp_timer_get_time(void) { return s_now_us; }

void sensor_bus_lines_read(bool *sda_high, bool *scl_high)
{
    *sda_high = !s_after_transfer;
    *scl_high = true;
}

void fake_sensor_bus_log(const char *tag, const char *format, ...)
{
    va_list args;
    assert(!s_owner_held);
    assert(strcmp(tag, "I2C_PROBE") == 0);
    va_start(args, format);
    (void)vsnprintf(s_last_line, sizeof(s_last_line), format, args);
    va_end(args);
    if (strstr(s_last_line, "transactions_dropped=") != NULL) ++s_overflow;
    ++s_lines;
    s_now_us += 50000; /* Slow UART must not consume an I2C owner budget. */
}

static esp_err_t transfer(void *context, const uint8_t *tx, size_t tx_len,
                          uint8_t *rx, size_t rx_len)
{
    (void)context;
    assert(tx != NULL && tx_len == 1U);
    assert(rx != NULL && rx_len == 1U);
    *rx = *tx;
    s_after_transfer = true;
    s_now_us += 1000;
    return s_transfer_result;
}

static void transact(uint8_t value)
{
    uint8_t rx = 0;
    s_after_transfer = false;
    assert(sensor_bus_probe_diag_transaction(
        SENSOR_BUS_PROBE_PHASE_TCA_VERIFY, 0x70U, 3, &value, 1U, &rx, 1U,
        true, value, transfer, NULL) == s_transfer_result);
    assert(rx == value);
}

int main(void)
{
    sensor_bus_probe_diag_begin_pass(1U);
    s_owner_held = true;
    sensor_bus_probe_diag_defer_output();
    const int64_t started_us = s_now_us;
    transact(0x08U);
    s_transfer_result = ESP_ERR_TIMEOUT;
    transact(0x00U);
    assert(s_lines == 0U);
    assert(s_now_us - started_us == 2000);
    s_owner_held = false;
    sensor_bus_probe_diag_flush();
#if CONFIG_APP_I2C_PROBE_DIAGNOSTICS
    assert(s_lines == 2U);
    assert(strstr(s_last_line, "tx=00 rx=- exp=00") != NULL);
    assert(strstr(s_last_line, "us=1000 lvl=SDA1/SCL1>01") != NULL);
    const unsigned lines_before = s_lines;
    sensor_bus_probe_diag_flush();
    assert(s_lines == lines_before);

    s_owner_held = true;
    sensor_bus_probe_diag_defer_output();
    s_transfer_result = ESP_OK;
    for (unsigned i = 0U; i < SENSOR_BUS_PROBE_DIAG_PENDING_LIMIT + 2U; ++i) {
        transact((uint8_t)i);
    }
    assert(s_lines == lines_before);
    s_owner_held = false;
    sensor_bus_probe_diag_flush();
    assert(s_lines == lines_before + SENSOR_BUS_PROBE_DIAG_PENDING_LIMIT + 1U);
    assert(s_overflow == 1U);
    assert(strstr(s_last_line, "transactions_dropped=2") != NULL);
    transact(0x80U);
    assert(s_lines == lines_before + SENSOR_BUS_PROBE_DIAG_PENDING_LIMIT + 2U);
    assert(strstr(s_last_line, "tx=80 rx=80 exp=80") != NULL);
#else
    assert(s_lines == 0U);
#endif
    sensor_bus_probe_diag_end_pass();
    puts("sensor_bus_probe_diag: PASS");
    return 0;
}
