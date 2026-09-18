#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


/*
 * Whole-object emergency-stop trigger (MAX-118): while a recording session
 * is active, ANY connected pod jumping more than 1 m in barometric altitude,
 * or shifting on all 3 accelerometer axes at once, means the geophone moved,
 * fell, or was disturbed -- stop the WHOLE recording session (every
 * geophone), not just that node. Per the user: "Детектор аварийной остановки
 * - останавливает запись на обьекте, т.е. останавливает запись на всех
 * геофонах подключённых к устройству."
 *
 * A small single-axis drift while a geophone sits on solid ground is normal
 * thermal/vibration noise and must NOT trigger -- only a simultaneous shift
 * on all 3 axes (a real jolt/tip-over) counts.
 *
 * Neither threshold below is a spec value or an HIL measurement -- both are
 * conservative, documented, tunable defaults pending real field data.
 */
#define NRF_EMERGENCY_STOP_ALTITUDE_THRESHOLD_MM 1000
#define NRF_EMERGENCY_STOP_ACCEL_THRESHOLD_MG      300


typedef struct
{
    bool valid;
    int32_t altitude_mm;
    int32_t accel_mg[3];
} nrf_emergency_stop_channel_sample_t;


/*
 * True if `current` differs from `baseline` enough on this one channel to
 * count as a fall/impact/move. False whenever either sample isn't `valid`
 * -- missing data must never manufacture a trigger.
 */
bool nrf_emergency_stop_channel_triggered(
    const nrf_emergency_stop_channel_sample_t *baseline,
    const nrf_emergency_stop_channel_sample_t *current
);


/*
 * Evaluate every channel in `baseline`/`current` (both arrays of `count`
 * entries, indexed the same way by the caller -- typically per mux
 * channel). Returns true on the first triggering channel and reports its
 * index via `triggered_channel` (may be NULL).
 */
bool nrf_emergency_stop_any_channel_triggered(
    const nrf_emergency_stop_channel_sample_t *baseline,
    const nrf_emergency_stop_channel_sample_t *current,
    size_t count,
    size_t *triggered_channel
);
