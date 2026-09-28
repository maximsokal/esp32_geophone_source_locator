#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(__has_include)
#if __has_include("seismic_acquisition.h")
#include "seismic_acquisition.h"
#else
typedef struct seismic_acquisition_snapshot seismic_acquisition_snapshot_t;
#endif
#else
typedef struct seismic_acquisition_snapshot seismic_acquisition_snapshot_t;
#endif

typedef struct _lv_obj_t lv_obj_t;

#define UI_MEASUREMENT_PREVIEW_SPAN_MS  1024U

static inline size_t ui_measurements_preview_frames_for_rate(uint32_t sample_rate_hz)
{
    if (sample_rate_hz == 0U) return 0U;
    const uint64_t frames =
        ((uint64_t)sample_rate_hz * UI_MEASUREMENT_PREVIEW_SPAN_MS + 500U) / 1000U;
    return frames > SIZE_MAX ? SIZE_MAX : (size_t)(frames == 0U ? 1U : frames);
}

void ui_measurements_create_charts_locked(lv_obj_t *screen, int y);
void ui_measurements_refresh_charts_locked(
    const seismic_acquisition_snapshot_t *acquisition);
