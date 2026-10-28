#pragma once

struct device;

int pm_device_runtime_get(const struct device *dev);
int pm_device_runtime_put(const struct device *dev);
