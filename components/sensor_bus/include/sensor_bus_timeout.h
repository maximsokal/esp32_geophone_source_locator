#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Software timeout policy for blocking I2C transfers.
 *
 * i2c_master waits for a transfer on a FreeRTOS queue for pdMS_TO_TICKS(timeout_ms)
 * ticks. A wait of N ticks that starts at an arbitrary point of a tick period
 * expires after between N-1 and N ticks, so at 100 Hz a nominal 20 ms is only
 * guaranteed to last 10 ms.
 *
 * Measured on target (logs/esp32, 2026-10-25 and 2026-10-26): a 136-byte read
 * from an nRF node takes 12.5-14.3 ms at 100 kHz (nominal 12.35 ms). With the
 * fixed 20 ms timeout 40 of the 50 such reads (REMOTE sensor snapshot and log
 * blocks) were aborted by the 10-20 ms window and each abort cost a further
 * ~110 ms of controller recovery.
 *
 * The policy grows the timeout, one tick at a time, until the guaranteed
 * minimum wait covers SENSOR_BUS_TIMEOUT_WIRE_MARGIN_PERCENT of the wire time.
 * Transfers that already fit (up to 72 bytes at 100 kHz and 100 Hz ticks) keep
 * the base timeout, so the owner hold budget (elapsed + timeout + mux close
 * reserve <= 150 ms) is unchanged for them; a 136-byte read asks for 30 ms.
 */
#define SENSOR_BUS_TIMEOUT_WIRE_MARGIN_PERCENT 150U
#define SENSOR_BUS_TIMEOUT_MAX_MS 60000U
#define SENSOR_BUS_TIMEOUT_MAX_BYTES (1U << 20)

/* START + address byte + data bytes at nine clocks each + STOP. */
static inline uint32_t sensor_bus_wire_time_us(size_t data_bytes, uint32_t speed_hz)
{
    if (speed_hz == 0U) {
        return UINT32_MAX;
    }
    if (data_bytes > SENSOR_BUS_TIMEOUT_MAX_BYTES) {
        data_bytes = SENSOR_BUS_TIMEOUT_MAX_BYTES;
    }
    const uint64_t bits = ((uint64_t)data_bytes + 1U) * 9U + 2U;
    const uint64_t us = (bits * 1000000U + speed_hz - 1U) / speed_hz;
    return us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}

/* Shortest real time a wait of pdMS_TO_TICKS(timeout_ms) ticks can last. */
static inline uint32_t sensor_bus_wait_floor_ms(uint32_t timeout_ms, uint32_t tick_hz)
{
    if (tick_hz == 0U || tick_hz > 1000U) {
        return 0U;
    }
    const uint64_t ticks = ((uint64_t)timeout_ms * tick_hz) / 1000U;
    if (ticks == 0U) {
        return 0U;
    }
    return (uint32_t)(((ticks - 1U) * 1000U) / tick_hz);
}

static inline uint32_t sensor_bus_transfer_timeout_ms(
    size_t data_bytes,
    uint32_t speed_hz,
    uint32_t tick_hz,
    uint32_t base_ms)
{
    const uint32_t tick_ms = (tick_hz == 0U || tick_hz > 1000U)
        ? 1U : (1000U + tick_hz - 1U) / tick_hz;
    const uint64_t required_ms =
        ((uint64_t)sensor_bus_wire_time_us(data_bytes, speed_hz) *
             SENSOR_BUS_TIMEOUT_WIRE_MARGIN_PERCENT + 99999U) / 100000U;
    uint32_t timeout_ms = base_ms;

    while (sensor_bus_wait_floor_ms(timeout_ms, tick_hz) < required_ms &&
           timeout_ms < SENSOR_BUS_TIMEOUT_MAX_MS) {
        timeout_ms += tick_ms;
    }
    return timeout_ms;
}
