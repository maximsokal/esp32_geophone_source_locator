#pragma once

#include <stdio.h>

unsigned esp_log_timestamp(void);

#define ESP_LOGI(...) do { if (0) { (void)printf(__VA_ARGS__); } } while (0)
#define ESP_LOGW(...) do { if (0) { (void)printf(__VA_ARGS__); } } while (0)
#define ESP_LOGE(...) do { if (0) { (void)printf(__VA_ARGS__); } } while (0)
