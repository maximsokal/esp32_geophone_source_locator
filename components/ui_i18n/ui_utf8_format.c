#include "ui_utf8_format.h"

#include <stdarg.h>
#include <stdio.h>

static size_t utf8_complete_prefix(const unsigned char *text, size_t length)
{
    size_t offset = 0U;
    while (offset < length) {
        const unsigned char lead = text[offset];
        size_t width = 0U;
        if (lead <= 0x7FU) {
            ++offset;
            continue;
        } else if (lead >= 0xC2U && lead <= 0xDFU) {
            width = 2U;
        } else if (lead >= 0xE0U && lead <= 0xEFU) {
            width = 3U;
        } else if (lead >= 0xF0U && lead <= 0xF4U) {
            width = 4U;
        } else {
            break;
        }

        if (length - offset < width) break;
        const unsigned char second = text[offset + 1U];
        if ((second & 0xC0U) != 0x80U ||
            (width >= 3U && (text[offset + 2U] & 0xC0U) != 0x80U) ||
            (width == 4U && (text[offset + 3U] & 0xC0U) != 0x80U) ||
            (lead == 0xE0U && second < 0xA0U) ||
            (lead == 0xEDU && second >= 0xA0U) ||
            (lead == 0xF0U && second < 0x90U) ||
            (lead == 0xF4U && second >= 0x90U)) {
            break;
        }
        offset += width;
    }
    return offset;
}

size_t ui_utf8_truncate(char *text, size_t cap)
{
    if (text == NULL || cap == 0U) return 0U;

    size_t length = 0U;
    while (length < cap - 1U && text[length] != '\0') ++length;
    text[length] = '\0';
    length = utf8_complete_prefix((const unsigned char *)text, length);
    text[length] = '\0';
    return length;
}

int ui_utf8_format(char *dst, size_t cap, const char *fmt, ...)
{
    if (fmt == NULL) {
        if (dst != NULL && cap > 0U) dst[0] = '\0';
        return -1;
    }

    va_list args;
    va_start(args, fmt);
    const int result = dst == NULL || cap == 0U
        ? vsnprintf(NULL, 0U, fmt, args)
        : vsnprintf(dst, cap, fmt, args);
    va_end(args);

    if (dst != NULL && cap > 0U) {
        if (result < 0) {
            dst[0] = '\0';
        } else if ((size_t)result >= cap) {
            (void)ui_utf8_truncate(dst, cap);
        }
    }
    return result;
}
