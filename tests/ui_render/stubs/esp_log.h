#pragma once
static inline void render_log(const char *format, ...) { (void)format; }
#define ESP_LOGI(tag, ...) ((void)(tag), render_log(__VA_ARGS__))
#define ESP_LOGW(tag, ...) ((void)(tag), render_log(__VA_ARGS__))
#define ESP_LOGE(tag, ...) ((void)(tag), render_log(__VA_ARGS__))
