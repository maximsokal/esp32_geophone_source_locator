#include "cJSON.h"
#include "cjson_alloc_guard.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Models the production hooks: a failed allocation inside the parser is
 * counted by cjson_alloc_guard exactly as cjson_alloc_guard_alloc() would.
 * cJSON_stub_fail_alloc_countdown: -1 = never fail, N >= 0 = fail the
 * allocation after N more successful ones. */
int cJSON_stub_fail_alloc_countdown = -1;

static void *stub_alloc(size_t size)
{
    if (cJSON_stub_fail_alloc_countdown == 0) {
        cJSON_stub_fail_alloc_countdown = -1;
        cjson_alloc_guard_note_failure();
        return NULL;
    }
    if (cJSON_stub_fail_alloc_countdown > 0) {
        --cJSON_stub_fail_alloc_countdown;
    }
    return calloc(1U, size);
}

static const char *skip_space(const char *cursor)
{
    while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
        ++cursor;
    }
    return cursor;
}

static char *parse_string(const char **cursor)
{
    const char *input = *cursor;
    if (*input != '"') {
        return NULL;
    }
    ++input;
    const char *start = input;
    size_t length = 0;
    while (*input != '\0' && *input != '"') {
        if (*input == '\\') {
            return NULL;
        }
        ++input;
        ++length;
    }
    if (*input != '"') {
        return NULL;
    }
    char *value = stub_alloc(length + 1U);
    if (value == NULL) {
        return NULL;
    }
    memcpy(value, start, length);
    value[length] = '\0';
    *cursor = input + 1;
    return value;
}

static void delete_item(cJSON *item)
{
    if (item == NULL) {
        return;
    }
    free(item->string);
    free(item->valuestring);
    free(item);
}

cJSON *cJSON_Parse(const char *text)
{
    if (text == NULL) {
        return NULL;
    }

    const char *cursor = skip_space(text);
    if (*cursor++ != '{') {
        return NULL;
    }

    cJSON *head = NULL;
    cJSON **tail = &head;
    cursor = skip_space(cursor);
    if (*cursor == '}') {
        return head;
    }

    while (*cursor != '\0') {
        char *key = parse_string(&cursor);
        if (key == NULL) {
            cJSON_Delete(head);
            return NULL;
        }
        cursor = skip_space(cursor);
        if (*cursor++ != ':') {
            free(key);
            cJSON_Delete(head);
            return NULL;
        }
        cursor = skip_space(cursor);

        cJSON *item = stub_alloc(sizeof(*item));
        if (item == NULL) {
            free(key);
            cJSON_Delete(head);
            return NULL;
        }
        item->string = key;
        if (*cursor == '"') {
            item->type = cJSON_String;
            item->valuestring = parse_string(&cursor);
            if (item->valuestring == NULL) {
                delete_item(item);
                cJSON_Delete(head);
                return NULL;
            }
        } else {
            errno = 0;
            char *end = NULL;
            long value = strtol(cursor, &end, 10);
            if (end == cursor || errno == ERANGE || value < INT_MIN || value > INT_MAX) {
                delete_item(item);
                cJSON_Delete(head);
                return NULL;
            }
            item->type = cJSON_Number;
            item->valueint = (int)value;
            item->valuedouble = (double)value;
            cursor = end;
        }

        *tail = item;
        tail = &item->next;
        cursor = skip_space(cursor);
        if (*cursor == '}') {
            return head;
        }
        if (*cursor++ != ',') {
            cJSON_Delete(head);
            return NULL;
        }
        cursor = skip_space(cursor);
    }

    cJSON_Delete(head);
    return NULL;
}

cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *object, const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (const cJSON *item = object; item != NULL; item = item->next) {
        if (strcmp(item->string, name) == 0) {
            return (cJSON *)item;
        }
    }
    return NULL;
}

bool cJSON_IsNumber(const cJSON *item)
{
    return item != NULL && item->type == cJSON_Number;
}

bool cJSON_IsString(const cJSON *item)
{
    return item != NULL && item->type == cJSON_String;
}

void cJSON_Delete(cJSON *item)
{
    while (item != NULL) {
        cJSON *next = item->next;
        delete_item(item);
        item = next;
    }
}
