#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "sensor_bus_timeout.h"

/*
 * Pure contract of the software I2C timeout policy.
 *
 * Measured on the board (logs/esp32/esp32_COM3_2026-10-25_23-15-23.log and
 * ..._2026-10-26_10-01-35.log): a 136-byte read from an nRF target at 100 kHz
 * lasted 12.5-14.3 ms when it succeeded. Fifty such reads were attempted, ten
 * succeeded; the others were cut off by the fixed 20 ms timeout, which at a
 * 100 Hz tick is only guaranteed to last 10 ms.
 */
#define TARGET_SPEED_HZ 100000U
#define TARGET_TICK_HZ 100U
#define BASE_MS 20U
#define MEASURED_LONGEST_OK_136_BYTE_READ_US 14273U

static unsigned s_checks;
#define CHECK(condition) do { ++s_checks; assert(condition); } while (0)

static void test_wire_time_matches_i2c_framing(void)
{
    /* START + address + n data bytes: (n + 1) * 9 clocks, plus the stop condition. */
    CHECK(sensor_bus_wire_time_us(0U, 100000U) == 110U);
    CHECK(sensor_bus_wire_time_us(1U, 100000U) == 200U);
    CHECK(sensor_bus_wire_time_us(28U, 100000U) == 2630U);
    CHECK(sensor_bus_wire_time_us(38U, 100000U) == 3530U);
    CHECK(sensor_bus_wire_time_us(136U, 100000U) == 12350U);
    CHECK(sensor_bus_wire_time_us(136U, 400000U) == 3088U);
    /* The model must not understate the measured transfers (12.5 ms and above). */
    CHECK(sensor_bus_wire_time_us(136U, TARGET_SPEED_HZ) < MEASURED_LONGEST_OK_136_BYTE_READ_US);
    CHECK(MEASURED_LONGEST_OK_136_BYTE_READ_US * 100U / sensor_bus_wire_time_us(136U, TARGET_SPEED_HZ) < 120U);
}

static void test_invalid_inputs_are_bounded(void)
{
    CHECK(sensor_bus_wire_time_us(10U, 0U) == UINT32_MAX);
    CHECK(sensor_bus_wire_time_us(SIZE_MAX, 100000U) == sensor_bus_wire_time_us(SENSOR_BUS_TIMEOUT_MAX_BYTES, 100000U));
    CHECK(sensor_bus_wire_time_us(SIZE_MAX, 1U) == UINT32_MAX);
    CHECK(sensor_bus_wait_floor_ms(100U, 0U) == 0U);
    CHECK(sensor_bus_wait_floor_ms(100U, 1001U) == 0U);
    CHECK(sensor_bus_wait_floor_ms(0U, 100U) == 0U);
    CHECK(sensor_bus_wait_floor_ms(9U, 100U) == 0U);   /* pdMS_TO_TICKS(9) == 0: no wait at all */
    CHECK(sensor_bus_wait_floor_ms(10U, 100U) == 0U);  /* one tick may expire immediately */
    CHECK(sensor_bus_wait_floor_ms(20U, 100U) == 10U); /* the documented 10-20 ms window */
    CHECK(sensor_bus_wait_floor_ms(40U, 100U) == 30U);
    CHECK(sensor_bus_wait_floor_ms(UINT32_MAX, 1000U) == UINT32_MAX - 1U);
    /* An unusable speed or tick base must still terminate and stay bounded. */
    CHECK(sensor_bus_transfer_timeout_ms(8U, 0U, 100U, BASE_MS) >= SENSOR_BUS_TIMEOUT_MAX_MS);
    CHECK(sensor_bus_transfer_timeout_ms(8U, 0U, 100U, BASE_MS) <= SENSOR_BUS_TIMEOUT_MAX_MS + 10U);
    CHECK(sensor_bus_transfer_timeout_ms(8U, 100000U, 0U, BASE_MS) >= SENSOR_BUS_TIMEOUT_MAX_MS);
    CHECK(sensor_bus_transfer_timeout_ms(8U, 100000U, 100U, 0U) >= 20U);
}

static void test_old_fixed_timeout_is_too_short_for_the_measured_reads(void)
{
    /* This is the defect: 20 ms @ 100 Hz is guaranteed for only 10 ms. */
    CHECK(sensor_bus_wait_floor_ms(BASE_MS, TARGET_TICK_HZ) == 10U);
    CHECK(sensor_bus_wait_floor_ms(BASE_MS, TARGET_TICK_HZ) * 1000U <
          MEASURED_LONGEST_OK_136_BYTE_READ_US);
    CHECK(sensor_bus_wait_floor_ms(BASE_MS, TARGET_TICK_HZ) * 1000U <
          sensor_bus_wire_time_us(136U, TARGET_SPEED_HZ));
}

static void test_target_values(void)
{
    /* Everything the firmware reads in up to 72 bytes keeps its 20 ms. */
    for (size_t bytes = 1U; bytes <= 72U; ++bytes) {
        CHECK(sensor_bus_transfer_timeout_ms(bytes, TARGET_SPEED_HZ, TARGET_TICK_HZ, BASE_MS) == BASE_MS);
    }
    CHECK(sensor_bus_transfer_timeout_ms(73U, TARGET_SPEED_HZ, TARGET_TICK_HZ, BASE_MS) == 30U);
    CHECK(sensor_bus_transfer_timeout_ms(96U, TARGET_SPEED_HZ, TARGET_TICK_HZ, BASE_MS) == 30U);
    /* REMOTE sensor snapshot and the largest log block. */
    CHECK(sensor_bus_transfer_timeout_ms(136U, TARGET_SPEED_HZ, TARGET_TICK_HZ, BASE_MS) == 30U);
    CHECK(sensor_bus_wait_floor_ms(30U, TARGET_TICK_HZ) * 1000U >=
          MEASURED_LONGEST_OK_136_BYTE_READ_US + 5000U);
    /* 20 ms must stay short of the measured transfer, 30 ms must clear it. */
    CHECK(sensor_bus_wait_floor_ms(20U, TARGET_TICK_HZ) * 1000U < MEASURED_LONGEST_OK_136_BYTE_READ_US);
    CHECK(sensor_bus_wait_floor_ms(30U, TARGET_TICK_HZ) * 1000U > MEASURED_LONGEST_OK_136_BYTE_READ_US);
}

static void test_guarantee_holds_for_every_length_speed_and_tick(void)
{
    static const uint32_t speeds[] = {10000U, 50000U, 100000U, 400000U, 1000000U};
    static const uint32_t ticks[] = {100U, 200U, 250U, 500U, 1000U};
    static const uint32_t bases[] = {1U, 20U, 50U};

    for (size_t s = 0U; s < sizeof(speeds) / sizeof(speeds[0]); ++s) {
        for (size_t t = 0U; t < sizeof(ticks) / sizeof(ticks[0]); ++t) {
            for (size_t b = 0U; b < sizeof(bases) / sizeof(bases[0]); ++b) {
                uint32_t previous = 0U;
                for (size_t bytes = 0U; bytes <= 512U; ++bytes) {
                    const uint32_t timeout = sensor_bus_transfer_timeout_ms(
                        bytes, speeds[s], ticks[t], bases[b]);
                    const uint64_t required_ms =
                        ((uint64_t)sensor_bus_wire_time_us(bytes, speeds[s]) *
                             SENSOR_BUS_TIMEOUT_WIRE_MARGIN_PERCENT + 99999U) / 100000U;
                    const uint32_t tick_ms = (1000U + ticks[t] - 1U) / ticks[t];

                    /* Never shorter than the caller's base, monotonic in size. */
                    CHECK(timeout >= bases[b]);
                    CHECK(timeout >= previous);
                    previous = timeout;
                    /* The shortest possible wait covers the margin times the wire time. */
                    if (timeout < SENSOR_BUS_TIMEOUT_MAX_MS) {
                        CHECK(sensor_bus_wait_floor_ms(timeout, ticks[t]) >= required_ms);
                        /* Minimal: one tick less would not satisfy it, unless the base already does. */
                        if (timeout > bases[b]) {
                            CHECK(sensor_bus_wait_floor_ms(timeout - tick_ms, ticks[t]) < required_ms);
                        }
                    }
                }
            }
        }
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_wire_time_matches_i2c_framing();
    test_invalid_inputs_are_bounded();
    test_old_fixed_timeout_is_too_short_for_the_measured_reads();
    test_target_values();
    test_guarantee_holds_for_every_length_speed_and_tick();
    printf("sensor_bus_timeout_policy: %u checks passed\n", s_checks);
    return 0;
}
