#pragma once

void fake_runtime_log(const char *level, const char *tag, const char *format, ...);

#define ESP_LOGE(tag, format, ...) fake_runtime_log("E", (tag), (format), ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) fake_runtime_log("W", (tag), (format), ##__VA_ARGS__)
#define ESP_LOGI(tag, format, ...) fake_runtime_log("I", (tag), (format), ##__VA_ARGS__)
