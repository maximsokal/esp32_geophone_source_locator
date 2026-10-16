#pragma once

#include <stdarg.h>

unsigned esp_log_timestamp(void);
#ifdef OBJECT_RECOVERY_CAPTURE_LOGS
void object_recovery_test_log(const char *format, ...);

#define ESP_LOGI(tag, ...) do { (void)(tag); object_recovery_test_log(__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, ...) do { (void)(tag); object_recovery_test_log(__VA_ARGS__); } while (0)
#define ESP_LOGE(tag, ...) do { (void)(tag); object_recovery_test_log(__VA_ARGS__); } while (0)
#else
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
#endif
