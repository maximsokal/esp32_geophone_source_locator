#pragma once

void fake_esp_log(const char *tag, const char *format, ...);

#define ESP_LOGE(tag, format, ...) fake_esp_log((tag), (format), ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) fake_esp_log((tag), (format), ##__VA_ARGS__)
#define ESP_LOGI(tag, format, ...) fake_esp_log((tag), (format), ##__VA_ARGS__)
