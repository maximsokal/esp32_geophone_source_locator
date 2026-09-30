#ifndef UI_UTF8_FORMAT_H
#define UI_UTF8_FORMAT_H

#include <stddef.h>

#if defined(__GNUC__) || defined(__clang__)
#define UI_UTF8_PRINTF_FORMAT(format_index, first_argument) \
    __attribute__((format(printf, format_index, first_argument)))
#else
#define UI_UTF8_PRINTF_FORMAT(format_index, first_argument)
#endif

/*
 * Formats like snprintf and returns its required byte count excluding NUL,
 * or a negative value on formatting error. If dst is non-NULL and cap > 0,
 * output is NUL-terminated; truncated output ends at a complete UTF-8 code
 * point. The return count is before UTF-8 trimming. A NULL dst or zero cap
 * writes nothing.
 */
int ui_utf8_format(char *dst, size_t cap, const char *fmt, ...)
    UI_UTF8_PRINTF_FORMAT(3, 4);

/* NUL-terminates within cap, trims invalid/incomplete UTF-8, and returns the retained byte count. */
size_t ui_utf8_truncate(char *text, size_t cap);

#undef UI_UTF8_PRINTF_FORMAT

#endif
