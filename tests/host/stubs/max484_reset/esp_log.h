#pragma once

void max484_test_log(const char *tag, const char *format, ...);

#define ESP_LOGI(tag, ...) max484_test_log(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) max484_test_log(tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) max484_test_log(tag, __VA_ARGS__)
