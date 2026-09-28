#pragma once

#include "lvgl.h"
#include "seismic_acquisition.h"

void ui_measurements_create_charts_locked(lv_obj_t *screen, int y);
void ui_measurements_refresh_charts_locked(
    const seismic_acquisition_snapshot_t *acquisition);
