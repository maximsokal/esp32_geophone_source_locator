#pragma once

void fake_sensor_bus_log(const char *tag, const char *format, ...);

#define ESP_LOGI(...) fake_sensor_bus_log(__VA_ARGS__)
#define ESP_LOGW(...) fake_sensor_bus_log(__VA_ARGS__)
#define ESP_LOGE(...) fake_sensor_bus_log(__VA_ARGS__)
