#include "seismic_rate_profile.h"

/* 500 Hz is the original profile and has no limit: the SD path uses ~10 % of its time. */
static const seismic_rate_profile_t s_profiles[] = {
    { 500U, 0U },
    { 2000U, 120U },
    { 4000U, 30U },
    { 5000U, 25U },
    /* Above 4 kHz the SD path falls behind and only the buffers hold the excess: 10 kHz fills them in ~7.7 s, 15 kHz in ~5.1 s. */
    { 10000U, 5U },
    { 15000U, 3U },
};

/* Frames the write-behind cache holds (CONFIG_SEISMIC_RECORDER_PSRAM_CACHE_KB = 1024 / 40-byte slots). */
#define CACHE_FRAMES 26214U
/* The ring holds two seconds of frames, at most what its 2 MiB budget fits at 44 bytes per frame. */
#define RING_SECONDS 2U
#define RING_MAX_FRAMES 47662U

size_t seismic_rate_profile_count(void)
{
    return sizeof(s_profiles) / sizeof(s_profiles[0]);
}


const seismic_rate_profile_t *seismic_rate_profile_at(size_t index)
{
    return index < seismic_rate_profile_count() ? &s_profiles[index] : NULL;
}


const seismic_rate_profile_t *seismic_rate_profile_find(uint32_t sample_rate_hz)
{
    for (size_t i = 0U; i < seismic_rate_profile_count(); ++i) {
        if (s_profiles[i].sample_rate_hz == sample_rate_hz) return &s_profiles[i];
    }
    return NULL;
}


uint32_t seismic_rate_profile_default_hz(void)
{
    return s_profiles[0].sample_rate_hz;
}


uint32_t seismic_rate_profile_record_limit_ms(uint32_t sample_rate_hz)
{
    const seismic_rate_profile_t *profile = seismic_rate_profile_find(sample_rate_hz);
    if (profile != NULL) return profile->max_record_seconds * 1000U;
    if (sample_rate_hz <= SEISMIC_RATE_PROFILE_PREVIEW_HZ) return 0U;

    /* Not a preset. At or below what the SD sustains the 4 kHz preset's limit applies; above it, three quarters of the
     * time the buffers take to fill, at least one second. */
    if (sample_rate_hz <= SEISMIC_RATE_PROFILE_SD_SUSTAINED_FPS) {
        return seismic_rate_profile_find(4000U)->max_record_seconds * 1000U;
    }
    uint64_t ring_frames = (uint64_t)sample_rate_hz * RING_SECONDS;
    if (ring_frames > RING_MAX_FRAMES) ring_frames = RING_MAX_FRAMES;
    const uint64_t buffered_frames = CACHE_FRAMES + ring_frames;
    const uint64_t excess_fps = (uint64_t)sample_rate_hz - SEISMIC_RATE_PROFILE_SD_SUSTAINED_FPS;
    uint64_t limit_ms = buffered_frames * 1000ULL * 3ULL / (excess_fps * 4ULL);
    if (limit_ms < 1000ULL) limit_ms = 1000ULL;
    return (uint32_t)limit_ms;
}


uint32_t seismic_rate_profile_preview_decimation(uint32_t sample_rate_hz)
{
    if (sample_rate_hz <= SEISMIC_RATE_PROFILE_PREVIEW_HZ) return 1U;
    return (sample_rate_hz + SEISMIC_RATE_PROFILE_PREVIEW_HZ - 1U) / SEISMIC_RATE_PROFILE_PREVIEW_HZ;
}
