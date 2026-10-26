#pragma once

#include <stdbool.h>

struct device { int unused; };
extern const struct device test_i2c30;
bool device_is_ready(const struct device *device);

#define DEVICE_DT_GET(node_id) DEVICE_DT_GET_IMPL(node_id)
#define DEVICE_DT_GET_IMPL(node_id) (&test_##node_id)
