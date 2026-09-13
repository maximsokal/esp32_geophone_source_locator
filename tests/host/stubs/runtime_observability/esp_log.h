#pragma once

void fake_runtime_log(const char *level, const char *tag, const char *format, ...);

/* The format string is always present, so place it in __VA_ARGS__ and avoid
 * compiler-specific comma swallowing for an otherwise empty variadic tail. */
#define ESP_LOGE(tag, ...) fake_runtime_log("E", (tag), __VA_ARGS__)
#define ESP_LOGW(tag, ...) fake_runtime_log("W", (tag), __VA_ARGS__)
#define ESP_LOGI(tag, ...) fake_runtime_log("I", (tag), __VA_ARGS__)
