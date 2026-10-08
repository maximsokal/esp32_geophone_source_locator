#pragma once

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline const char *localization_results_json_skip_space(const char *text)
{
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        ++text;
    }
    return text;
}


static inline const char *localization_results_json_skip_string(const char *text)
{
    if (*text != '"') return NULL;
    ++text;
    while (*text != '\0') {
        if (*text == '"') return text + 1;
        if (*text == '\\') {
            if (text[1] == '\0') return NULL;
            text += 2;
        } else {
            ++text;
        }
    }
    return NULL;
}


static inline const char *localization_results_json_skip_value(const char *text)
{
    text = localization_results_json_skip_space(text);
    if (*text == '"') return localization_results_json_skip_string(text);
    if (*text != '{' && *text != '[') {
        while (*text != '\0' && *text != ',' && *text != '}' && *text != ']' &&
               *text != ' ' && *text != '\t' && *text != '\r' && *text != '\n') {
            ++text;
        }
        return text;
    }

    size_t depth = 0U;
    while (*text != '\0') {
        if (*text == '"') {
            text = localization_results_json_skip_string(text);
            if (text == NULL) return NULL;
            continue;
        }
        if (*text == '{' || *text == '[') {
            ++depth;
        } else if (*text == '}' || *text == ']') {
            if (depth == 0U) return NULL;
            --depth;
            if (depth == 0U) return text + 1;
        }
        ++text;
    }
    return NULL;
}


static inline bool localization_results_json_key_matches(
    const char *key_start,
    const char *key_end,
    const char *name
)
{
    if (key_start == NULL || key_end == NULL || key_end - key_start < 2) return false;
    const size_t key_length = (size_t)(key_end - key_start - 2);
    return memchr(key_start + 1, '\\', key_length) == NULL &&
        key_length == strlen(name) && memcmp(key_start + 1, name, key_length) == 0;
}


static inline bool localization_results_json_find_root_value(
    const char *json,
    const char *name,
    const char **value_out,
    bool *present_out
)
{
    if (json == NULL || name == NULL || value_out == NULL || present_out == NULL) {
        return false;
    }
    *value_out = NULL;
    *present_out = false;

    const char *text = localization_results_json_skip_space(json);
    if (*text != '{') return false;
    text = localization_results_json_skip_space(text + 1);
    while (*text != '\0' && *text != '}') {
        const char *key_start = text;
        const char *key_end = localization_results_json_skip_string(text);
        if (key_end == NULL) return false;
        text = localization_results_json_skip_space(key_end);
        if (*text != ':') return false;
        const char *value = localization_results_json_skip_space(text + 1);
        if (localization_results_json_key_matches(key_start, key_end, name)) {
            *value_out = value;
            *present_out = true;
            return true;
        }

        text = localization_results_json_skip_value(value);
        if (text == NULL) return false;
        text = localization_results_json_skip_space(text);
        if (*text == ',') {
            text = localization_results_json_skip_space(text + 1);
        } else if (*text != '}') {
            return false;
        }
    }
    return *text == '}';
}


static inline bool localization_results_json_read_int64(
    const char *json,
    const char *name,
    int64_t *value_out,
    bool *present_out,
    bool *is_null_out
)
{
    if (value_out == NULL || present_out == NULL || is_null_out == NULL) return false;
    const char *value = NULL;
    if (!localization_results_json_find_root_value(json, name, &value, present_out)) {
        return false;
    }
    *is_null_out = false;
    if (!*present_out) return true;
    if (strncmp(value, "null", sizeof("null") - 1U) == 0) {
        const char *after_null = localization_results_json_skip_space(value + 4);
        if (*after_null != ',' && *after_null != '}') return false;
        *is_null_out = true;
        return true;
    }

    errno = 0;
    char *end = NULL;
    const long long parsed = strtoll(value, &end, 10);
    if (end == value || errno == ERANGE) return false;
    const char *after_number = localization_results_json_skip_space(end);
    if (*after_number != ',' && *after_number != '}') return false;
    *value_out = (int64_t)parsed;
    return true;
}


static inline bool localization_results_json_replace_int64(
    char **json_in_out,
    const char *name,
    int64_t value
)
{
    if (json_in_out == NULL || *json_in_out == NULL || name == NULL) return false;

    char needle[64];
    const int needle_length = snprintf(needle, sizeof(needle), "\"%s\":", name);
    if (needle_length < 0 || needle_length >= (int)sizeof(needle)) return false;
    char *member = strstr(*json_in_out, needle);
    if (member == NULL) return false;

    char *number_start = member + needle_length;
    while (*number_start == ' ' || *number_start == '\t' || *number_start == '\r' ||
           *number_start == '\n') {
        ++number_start;
    }
    char *number_end = number_start;
    while (*number_end != '\0' && *number_end != ',' && *number_end != '}' &&
           *number_end != ']' && *number_end != ' ' && *number_end != '\t' &&
           *number_end != '\r' && *number_end != '\n') {
        ++number_end;
    }
    if (number_end == number_start) return false;

    char replacement[32];
    const int replacement_length = snprintf(
        replacement, sizeof(replacement), "%lld", (long long)value
    );
    if (replacement_length < 0 || replacement_length >= (int)sizeof(replacement)) {
        return false;
    }

    const size_t json_length = strlen(*json_in_out);
    const size_t offset_start = (size_t)(number_start - *json_in_out);
    const size_t offset_end = (size_t)(number_end - *json_in_out);
    const size_t old_number_length = offset_end - offset_start;
    const size_t new_number_length = (size_t)replacement_length;
    const size_t new_json_length = json_length - old_number_length + new_number_length;
    if (new_json_length > json_length) {
        char *expanded = realloc(*json_in_out, new_json_length + 1U);
        if (expanded == NULL) return false;
        *json_in_out = expanded;
    }

    memmove(
        *json_in_out + offset_start + new_number_length,
        *json_in_out + offset_end,
        json_length - offset_end + 1U
    );
    memcpy(*json_in_out + offset_start, replacement, new_number_length);
    return true;
}
