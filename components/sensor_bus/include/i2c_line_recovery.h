#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * MAX-243: I2C stuck-bus handling per NXP UM10204 (I2C-bus specification,
 * "bus clear"). Pure algorithm over an abstract open-drain line interface,
 * host-tested against a simulated bus.
 *
 *  - SCL held LOW (beyond the clock-stretch allowance): the controller can
 *    not generate clocks -- the holder is a target (clock stretching that
 *    never ends, brownout). Report SCL_STUCK and do NOT toggle anything;
 *    only a reset/power cycle of that target can clear it.
 *  - SDA held LOW with SCL free: a target is mid-byte. Clock SCL up to 9
 *    times until it releases SDA, then generate a STOP and verify both
 *    lines are high.
 */

#define I2C_LINE_MAX_CLEAR_CLOCKS 9U
#define I2C_LINE_SCL_STRETCH_WAIT_US 1000U
#define I2C_LINE_POLL_US 10U
#define I2C_LINE_HALF_PERIOD_US 5U  /* 100 kHz bit-bang */

typedef struct
{
    bool (*read_sda)(void *context);
    bool (*read_scl)(void *context);
    /* true = release (open-drain high via pull-up), false = drive LOW. */
    void (*set_sda)(void *context, bool released);
    void (*set_scl)(void *context, bool released);
    void (*delay_us)(void *context, uint32_t us);
    void *context;

} i2c_line_io_t;

typedef enum
{
    I2C_LINES_IDLE = 0,     /* both lines high: nothing was done */
    I2C_LINES_RECOVERED,    /* SDA was stuck; clocks + STOP released it */
    I2C_LINES_SDA_STUCK,    /* SDA still low after 9 clocks */
    I2C_LINES_SCL_STUCK,    /* SCL held low: no clocks were generated */

} i2c_line_result_t;

typedef struct
{
    i2c_line_result_t result;
    uint8_t clocks;

} i2c_line_report_t;

i2c_line_report_t i2c_line_recover(const i2c_line_io_t *io);

/*
 * Bounded backoff for runtime recovery attempts: at least 1 s after the
 * previous attempt, doubling per consecutive failed attempt up to 60 s, so
 * a permanently stuck bus is not hammered.
 */
bool i2c_line_recovery_backoff_allows(
    int64_t now_us,
    int64_t last_attempt_us,
    uint32_t consecutive_failures
);

const char *i2c_line_result_name(i2c_line_result_t result);
