#pragma once

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*
 * Log lines are kept in one buffer shared by every translation unit of a test, so a test can assert what the
 * code under test logged. The buffer stops growing when full; a test that needs it clears it first.
 */
#define WORKFLOW_TEST_LOG_CAPACITY 16384U
#if defined(_MSC_VER)
#define WORKFLOW_TEST_LOG_SHARED __declspec(selectany)
#else
#define WORKFLOW_TEST_LOG_SHARED __attribute__((weak))
#endif
WORKFLOW_TEST_LOG_SHARED char workflow_test_log_buffer[WORKFLOW_TEST_LOG_CAPACITY] = {0};
WORKFLOW_TEST_LOG_SHARED size_t workflow_test_log_length = 0U;

static inline void workflow_test_log_clear(void)
{
    workflow_test_log_length = 0U;
    workflow_test_log_buffer[0] = '\0';
}

static inline void workflow_test_log_noop(const char *tag, const char *format, ...)
{
    (void)tag;
    if (workflow_test_log_length + 2U >= WORKFLOW_TEST_LOG_CAPACITY) return;
    va_list arguments;
    va_start(arguments, format);
    const int written = vsnprintf(
        &workflow_test_log_buffer[workflow_test_log_length],
        WORKFLOW_TEST_LOG_CAPACITY - workflow_test_log_length - 2U,
        format,
        arguments);
    va_end(arguments);
    if (written > 0) {
        const size_t room = WORKFLOW_TEST_LOG_CAPACITY - workflow_test_log_length - 2U;
        workflow_test_log_length += (size_t)written < room ? (size_t)written : room - 1U;
        workflow_test_log_buffer[workflow_test_log_length++] = '\n';
        workflow_test_log_buffer[workflow_test_log_length] = '\0';
    }
}

#define ESP_LOGI(tag, ...) workflow_test_log_noop((tag), __VA_ARGS__)
#define ESP_LOGW(tag, ...) workflow_test_log_noop((tag), __VA_ARGS__)
