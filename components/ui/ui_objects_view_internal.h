#pragma once

#include <stddef.h>

#include "esp_err.h"

#include "object_service.h"


#define OBJECT_LIST_VISIBLE      5

#define OBJECT_LIST_X            18
#define OBJECT_LIST_Y            67
#define OBJECT_LIST_WIDTH        284
#define OBJECT_LIST_HEIGHT       140
#define OBJECT_ROW_Y             70
#define OBJECT_ROW_STEP          28
#define OBJECT_ROW_HEIGHT        22


esp_err_t ui_objects_view_draw_active_line(
    const object_service_summary_t *active_object
);


size_t ui_objects_view_list_start(
    size_t object_count,
    size_t selected
);


esp_err_t ui_objects_view_draw_list_row(
    const object_service_summary_t *objects,
    size_t object_count,
    size_t start,
    size_t index,
    size_t selected
);


esp_err_t ui_objects_view_draw_list_rows(
    const object_service_summary_t *objects,
    size_t object_count,
    size_t selected
);
