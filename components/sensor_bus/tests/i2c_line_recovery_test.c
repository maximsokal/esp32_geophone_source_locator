#include <assert.h>
#include <string.h>

#include "i2c_line_recovery.h"

/*
 * MAX-243: simulated open-drain bus. A line is high only when the
 * controller releases it AND no target holds it low. A "stuck" target
 * holds SDA low until it has seen `sda_release_after_clocks` rising SCL
 * edges (mid-byte target finishing its byte), or forever (-1).
 */
typedef struct
{
    bool ctrl_sda_released;
    bool ctrl_scl_released;
    int sda_release_after_clocks; /* 0 = not holding, -1 = forever */
    bool scl_held_forever;
    int scl_held_for_us;          /* finite clock stretch */
    int rising_edges;
    int scl_driven_low_count;
    int stop_conditions;
    uint32_t elapsed_us;
    bool last_scl;
    bool last_sda;

} bus_t;

static bool target_holds_sda(const bus_t *bus)
{
    return bus->sda_release_after_clocks < 0 ||
        bus->rising_edges < bus->sda_release_after_clocks;
}

static bool line_scl(const bus_t *bus)
{
    return bus->ctrl_scl_released && !bus->scl_held_forever && bus->scl_held_for_us <= 0;
}

static bool line_sda(const bus_t *bus)
{
    return bus->ctrl_sda_released && !target_holds_sda(bus);
}

static void observe(bus_t *bus)
{
    const bool scl = line_scl(bus);
    const bool sda = line_sda(bus);
    if (scl && !bus->last_scl) {
        ++bus->rising_edges;
    }
    if (scl && bus->last_scl && sda && !bus->last_sda) {
        ++bus->stop_conditions;
    }
    bus->last_scl = line_scl(bus);
    bus->last_sda = line_sda(bus);
}

static bool read_sda(void *ctx) { return line_sda((bus_t *)ctx); }
static bool read_scl(void *ctx) { return line_scl((bus_t *)ctx); }

static void set_sda(void *ctx, bool released)
{
    bus_t *bus = ctx;
    bus->ctrl_sda_released = released;
    observe(bus);
}

static void set_scl(void *ctx, bool released)
{
    bus_t *bus = ctx;
    if (!released) {
        /* Clocking against a target that holds SCL is exactly what the
         * algorithm must never do. */
        assert(!bus->scl_held_forever);
        ++bus->scl_driven_low_count;
    }
    bus->ctrl_scl_released = released;
    observe(bus);
}

static void delay_us(void *ctx, uint32_t us)
{
    bus_t *bus = ctx;
    bus->elapsed_us += us;
    if (bus->scl_held_for_us > 0) {
        bus->scl_held_for_us -= (int)us;
    }
    observe(bus);
}

static i2c_line_io_t io_for(bus_t *bus)
{
    bus->last_scl = line_scl(bus);
    bus->last_sda = line_sda(bus);
    const i2c_line_io_t io = {
        .read_sda = read_sda, .read_scl = read_scl,
        .set_sda = set_sda, .set_scl = set_scl,
        .delay_us = delay_us, .context = bus,
    };
    return io;
}

static bus_t idle_bus(void)
{
    bus_t bus;
    memset(&bus, 0, sizeof(bus));
    bus.ctrl_sda_released = true;
    bus.ctrl_scl_released = true;
    return bus;
}

static void test_idle_bus_is_left_alone(void)
{
    bus_t bus = idle_bus();
    const i2c_line_io_t io = io_for(&bus);
    const i2c_line_report_t r = i2c_line_recover(&io);
    assert(r.result == I2C_LINES_IDLE);
    assert(r.clocks == 0U);
    assert(bus.scl_driven_low_count == 0);
}

static void test_sda_stuck_mid_byte_is_released_with_stop(void)
{
    for (int needed = 1; needed <= 9; ++needed) {
        bus_t bus = idle_bus();
        bus.sda_release_after_clocks = needed;
        const i2c_line_io_t io = io_for(&bus);
        const i2c_line_report_t r = i2c_line_recover(&io);
        assert(r.result == I2C_LINES_RECOVERED);
        assert(r.clocks == (uint8_t)needed);  /* stops clocking as soon as SDA frees */
        assert(bus.stop_conditions == 1);
        assert(line_sda(&bus) && line_scl(&bus));
        assert(bus.ctrl_sda_released && bus.ctrl_scl_released);
    }
}

static void test_sda_stuck_forever_gives_up_after_nine_clocks(void)
{
    bus_t bus = idle_bus();
    bus.sda_release_after_clocks = -1;
    const i2c_line_io_t io = io_for(&bus);
    const i2c_line_report_t r = i2c_line_recover(&io);
    assert(r.result == I2C_LINES_SDA_STUCK);
    assert(r.clocks == I2C_LINE_MAX_CLEAR_CLOCKS);
    assert(bus.scl_driven_low_count == (int)I2C_LINE_MAX_CLEAR_CLOCKS);
    /* Lines are released, never left driven by the controller. */
    assert(bus.ctrl_sda_released && bus.ctrl_scl_released);
}

static void test_scl_stuck_is_never_clocked(void)
{
    bus_t bus = idle_bus();
    bus.scl_held_forever = true;
    bus.sda_release_after_clocks = -1;
    const i2c_line_io_t io = io_for(&bus);
    const i2c_line_report_t r = i2c_line_recover(&io);
    assert(r.result == I2C_LINES_SCL_STUCK);
    assert(r.clocks == 0U);
    assert(bus.scl_driven_low_count == 0);
    /* Bounded wait: the stretch allowance plus one poll, not forever. */
    assert(bus.elapsed_us <= I2C_LINE_SCL_STRETCH_WAIT_US + 2U * I2C_LINE_POLL_US);
}

static void test_finite_clock_stretch_is_tolerated(void)
{
    bus_t bus = idle_bus();
    bus.scl_held_for_us = (int)(I2C_LINE_SCL_STRETCH_WAIT_US / 2U);
    bus.sda_release_after_clocks = 2;
    const i2c_line_io_t io = io_for(&bus);
    const i2c_line_report_t r = i2c_line_recover(&io);
    assert(r.result == I2C_LINES_RECOVERED);
}

static void test_missing_io_is_reported_as_fault(void)
{
    assert(i2c_line_recover(NULL).result == I2C_LINES_SCL_STUCK);
    i2c_line_io_t io;
    memset(&io, 0, sizeof(io));
    assert(i2c_line_recover(&io).result == I2C_LINES_SCL_STUCK);
}

static void test_backoff(void)
{
    const int64_t s = 1000000LL;
    assert(i2c_line_recovery_backoff_allows(5 * s, 0, 0U));          /* first attempt */
    assert(!i2c_line_recovery_backoff_allows(10 * s + s - 1, 10 * s, 0U));
    assert(i2c_line_recovery_backoff_allows(11 * s, 10 * s, 0U));
    assert(!i2c_line_recovery_backoff_allows(13 * s, 10 * s, 2U));   /* 4 s */
    assert(i2c_line_recovery_backoff_allows(14 * s, 10 * s, 2U));
    /* Capped at 60 s however many failures. */
    assert(!i2c_line_recovery_backoff_allows(69 * s, 10 * s, 1000U));
    assert(i2c_line_recovery_backoff_allows(70 * s, 10 * s, 1000U));
    assert(i2c_line_recovery_backoff_allows(70 * s, 10 * s, 0xFFFFFFFFU));
}

static void test_names(void)
{
    assert(strcmp(i2c_line_result_name(I2C_LINES_IDLE), "idle") == 0);
    assert(strcmp(i2c_line_result_name(I2C_LINES_RECOVERED), "recovered") == 0);
    assert(strcmp(i2c_line_result_name(I2C_LINES_SDA_STUCK), "sda_stuck") == 0);
    assert(strcmp(i2c_line_result_name(I2C_LINES_SCL_STUCK), "scl_stuck") == 0);
}

int main(void)
{
    test_idle_bus_is_left_alone();
    test_sda_stuck_mid_byte_is_released_with_stop();
    test_sda_stuck_forever_gives_up_after_nine_clocks();
    test_scl_stuck_is_never_clocked();
    test_finite_clock_stretch_is_tolerated();
    test_missing_io_is_reported_as_fault();
    test_backoff();
    test_names();
    return 0;
}
