#pragma once

#include <stdbool.h>

#include "workflow_core.h"

typedef enum {
    UI_MEASUREMENTS_ROUTE_ALLOW = 0,
    UI_MEASUREMENTS_ROUTE_WORKFLOW,
    UI_MEASUREMENTS_ROUTE_OBJECT_PICKER
} ui_measurements_route_t;

ui_measurements_route_t ui_measurements_route_decide(
    workflow_step_t workflow_step,
    bool has_active_object);

/* Steps where the live view is only for listening: OK/RIGHT must not start a recording from it. */
bool ui_measurements_listen_only(workflow_step_t workflow_step);
