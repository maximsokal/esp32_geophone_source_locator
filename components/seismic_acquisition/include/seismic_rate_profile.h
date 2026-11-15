#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Sampling-rate presets of a recording. The live preview (TFT, web) always shows the signal at about
 * SEISMIC_RATE_PROFILE_PREVIEW_HZ whatever the rate is, so only the recording runs faster.
 *
 * Each preset carries the longest recording the firmware can keep up with. The limits come from the ESP-EMU sweep of
 * 2026-11-15 with the board's SD write latency (docs/emulator-rate-sweep-2026-11-15.md): the SD path sustains about
 * 4000 frames/s; above that the ring (2 s) and the 26214-frame write-behind cache absorb the excess for
 *     (26214 + ring frames) / (rate - 4000) seconds,
 * after which frames are lost. The table keeps about a quarter of that margin. Up to 4 kHz the SD path keeps up, but at
 * 4 kHz it is busy 77..95 % of the time, so that preset is also limited.
 */
#define SEISMIC_RATE_PROFILE_PREVIEW_HZ 500U
#define SEISMIC_RATE_PROFILE_SD_SUSTAINED_FPS 4000U

typedef struct {
    uint32_t sample_rate_hz;
    uint32_t max_record_seconds;   /* 0: no limit */
} seismic_rate_profile_t;

size_t seismic_rate_profile_count(void);
const seismic_rate_profile_t *seismic_rate_profile_at(size_t index);
const seismic_rate_profile_t *seismic_rate_profile_find(uint32_t sample_rate_hz);

/* The rate the firmware starts with (the board's original 500 Hz). */
uint32_t seismic_rate_profile_default_hz(void);

/*
 * Longest recording at this rate in milliseconds, 0 for none. A rate that is not a preset (a build configured for
 * another rate) is limited by the same formula as the presets, never left unbounded above what the SD path sustains.
 */
uint32_t seismic_rate_profile_record_limit_ms(uint32_t sample_rate_hz);

/* Frames of the full-rate stream that make one frame of the preview. */
uint32_t seismic_rate_profile_preview_decimation(uint32_t sample_rate_hz);
