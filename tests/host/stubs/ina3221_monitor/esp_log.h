#pragma once

#define ESP_LOGI(tag, fmt, ...) do { (void)(tag); (void)sizeof(fmt); } while (0)
#define ESP_LOGW(tag, fmt, ...) do { (void)(tag); (void)sizeof(fmt); } while (0)
#define ESP_LOGE(tag, fmt, ...) do { (void)(tag); (void)sizeof(fmt); } while (0)
