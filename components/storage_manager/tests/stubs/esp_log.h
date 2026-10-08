#pragma once

#include <stdarg.h>
#include <stdio.h>

static char storage_manager_test_last_log_level;
static char storage_manager_test_last_log_message[512];


static void storage_manager_test_log(
    char level,
    const char *tag,
    const char *format,
    ...
)
{
    (void)tag;
    storage_manager_test_last_log_level = level;

    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(
        storage_manager_test_last_log_message,
        sizeof(storage_manager_test_last_log_message),
        format,
        arguments
    );
    va_end(arguments);
}


#define ESP_LOGD(tag, ...) storage_manager_test_log('D', (tag), __VA_ARGS__)
#define ESP_LOGI(tag, ...) storage_manager_test_log('I', (tag), __VA_ARGS__)
#define ESP_LOGW(tag, ...) storage_manager_test_log('W', (tag), __VA_ARGS__)
#define ESP_LOGE(tag, ...) storage_manager_test_log('E', (tag), __VA_ARGS__)

unsigned esp_log_timestamp(void);
