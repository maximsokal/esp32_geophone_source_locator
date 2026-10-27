#pragma once

void fake_sensor_bus_log(const char *tag, const char *format, ...);
void fake_sensor_bus_log_debug(const char *tag, const char *format, ...);

#define ESP_LOGI(...) fake_sensor_bus_log(__VA_ARGS__)
#define ESP_LOGD(...) fake_sensor_bus_log_debug(__VA_ARGS__)
#define ESP_LOGW(...) fake_sensor_bus_log(__VA_ARGS__)
#define ESP_LOGE(...) fake_sensor_bus_log(__VA_ARGS__)
