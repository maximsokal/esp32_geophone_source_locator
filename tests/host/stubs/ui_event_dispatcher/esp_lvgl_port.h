#pragma once

#include "esp_err.h"

#define LVGL_PORT_EVENT_USER 1

esp_err_t lvgl_port_task_wake(int event, void *data);
