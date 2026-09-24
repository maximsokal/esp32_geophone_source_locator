#include "i2c_line_recovery.h"

#include <stddef.h>


static bool wait_scl_high(const i2c_line_io_t *io)
{
    for (uint32_t waited = 0U; ; waited += I2C_LINE_POLL_US) {
        if (io->read_scl(io->context)) {
            return true;
        }
        if (waited >= I2C_LINE_SCL_STRETCH_WAIT_US) {
            return false;
        }
        io->delay_us(io->context, I2C_LINE_POLL_US);
    }
}


i2c_line_report_t i2c_line_recover(const i2c_line_io_t *io)
{
    i2c_line_report_t report = { .result = I2C_LINES_SCL_STUCK, .clocks = 0U };
    if (
        io == NULL || io->read_sda == NULL || io->read_scl == NULL ||
        io->set_sda == NULL || io->set_scl == NULL || io->delay_us == NULL
    ) {
        return report;
    }

    io->set_sda(io->context, true);
    io->set_scl(io->context, true);
    io->delay_us(io->context, I2C_LINE_HALF_PERIOD_US);

    if (!wait_scl_high(io)) {
        return report; /* SCL_STUCK: never clock against a held SCL */
    }
    if (io->read_sda(io->context)) {
        report.result = I2C_LINES_IDLE;
        return report;
    }

    while (report.clocks < I2C_LINE_MAX_CLEAR_CLOCKS && !io->read_sda(io->context)) {
        io->set_scl(io->context, false);
        io->delay_us(io->context, I2C_LINE_HALF_PERIOD_US);
        io->set_scl(io->context, true);
        ++report.clocks;
        if (!wait_scl_high(io)) {
            report.result = I2C_LINES_SCL_STUCK;
            return report;
        }
        io->delay_us(io->context, I2C_LINE_HALF_PERIOD_US);
    }

    if (!io->read_sda(io->context)) {
        report.result = I2C_LINES_SDA_STUCK;
        return report;
    }

    /* STOP: SDA rises while SCL is high. */
    io->set_scl(io->context, false);
    io->delay_us(io->context, I2C_LINE_HALF_PERIOD_US);
    io->set_sda(io->context, false);
    io->delay_us(io->context, I2C_LINE_HALF_PERIOD_US);
    io->set_scl(io->context, true);
    if (!wait_scl_high(io)) {
        report.result = I2C_LINES_SCL_STUCK;
        return report;
    }
    io->delay_us(io->context, I2C_LINE_HALF_PERIOD_US);
    io->set_sda(io->context, true);
    io->delay_us(io->context, I2C_LINE_HALF_PERIOD_US);

    report.result =
        io->read_sda(io->context) && io->read_scl(io->context)
        ? I2C_LINES_RECOVERED
        : I2C_LINES_SDA_STUCK;
    return report;
}


bool i2c_line_recovery_backoff_allows(
    int64_t now_us,
    int64_t last_attempt_us,
    uint32_t consecutive_failures
)
{
    if (last_attempt_us <= 0) {
        return true;
    }
    int64_t interval_us = 1000000LL;
    for (uint32_t i = 0U; i < consecutive_failures && interval_us < 60000000LL; ++i) {
        interval_us *= 2;
    }
    if (interval_us > 60000000LL) {
        interval_us = 60000000LL;
    }
    return now_us - last_attempt_us >= interval_us;
}


const char *i2c_line_result_name(i2c_line_result_t result)
{
    switch (result) {
        case I2C_LINES_IDLE:
            return "idle";
        case I2C_LINES_RECOVERED:
            return "recovered";
        case I2C_LINES_SDA_STUCK:
            return "sda_stuck";
        case I2C_LINES_SCL_STUCK:
            return "scl_stuck";
        default:
            return "unknown";
    }
}
