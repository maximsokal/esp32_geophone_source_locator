#pragma once

void fake_esp_log(const char *tag, const char *format, ...);

/* Keep the format string inside the variadic portion so every invocation has
 * at least one variadic argument. This avoids GNU comma-elision extensions in
 * strict/native C11 preprocessors while preserving the ESP_LOG call surface. */
#define ESP_LOGE(tag, ...) fake_esp_log((tag), __VA_ARGS__)
#define ESP_LOGW(tag, ...) fake_esp_log((tag), __VA_ARGS__)
#define ESP_LOGI(tag, ...) fake_esp_log((tag), __VA_ARGS__)
