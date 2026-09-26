#pragma once

void test_log_record(char level, const char *tag, const char *format, ...);

#define ESP_LOGI(tag, ...) test_log_record('I', tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) test_log_record('W', tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) test_log_record('E', tag, __VA_ARGS__)
