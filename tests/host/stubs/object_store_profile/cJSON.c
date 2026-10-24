#include "cJSON.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -1 = never fail; N >= 0 = the allocation after N more successful ones fails
 * (once), modelling an out-of-memory inside cJSON_Add*ToObject() and friends. */
int cJSON_stub_fail_alloc_countdown = -1;

static bool stub_allocation_fails(void)
{
    if (cJSON_stub_fail_alloc_countdown == 0) {
        cJSON_stub_fail_alloc_countdown = -1;
        return true;
    }
    if (cJSON_stub_fail_alloc_countdown > 0) {
        --cJSON_stub_fail_alloc_countdown;
    }
    return false;
}

static char *duplicate_text(const char *text)
{
    if (text == NULL) return NULL;
    if (stub_allocation_fails()) return NULL;
    const size_t length = strlen(text);
    char *copy = malloc(length + 1U);
    if (copy != NULL) memcpy(copy, text, length + 1U);
    return copy;
}

static cJSON *new_item(int type)
{
    if (stub_allocation_fails()) return NULL;
    cJSON *item = calloc(1U, sizeof(*item));
    if (item != NULL) item->type = type;
    return item;
}

static void delete_single(cJSON *item)
{
    if (item == NULL) return;
    cJSON *child = item->child;
    while (child != NULL) {
        cJSON *next = child->next;
        child->next = NULL;
        child->prev = NULL;
        delete_single(child);
        child = next;
    }
    free(item->string);
    free(item->valuestring);
    free(item);
}

void cJSON_Delete(cJSON *item)
{
    while (item != NULL) {
        cJSON *next = item->next;
        item->next = NULL;
        item->prev = NULL;
        delete_single(item);
        item = next;
    }
}

static const char *skip_space(const char *cursor)
{
    while (cursor != NULL && isspace((unsigned char)*cursor)) ++cursor;
    return cursor;
}

static char *parse_string(const char **cursor)
{
    if (cursor == NULL || *cursor == NULL || **cursor != '"') return NULL;
    ++*cursor;

    size_t capacity = 16U;
    size_t length = 0U;
    char *value = malloc(capacity);
    if (value == NULL) return NULL;

    while (**cursor != '\0' && **cursor != '"') {
        unsigned char character = (unsigned char)*(*cursor)++;
        if (character == '\\') {
            character = (unsigned char)*(*cursor)++;
            if (character == 'n') character = '\n';
            else if (character == 'r') character = '\r';
            else if (character == 't') character = '\t';
            else if (character != '"' && character != '\\' && character != '/') {
                free(value);
                return NULL;
            }
        }
        if (length + 1U >= capacity) {
            capacity *= 2U;
            char *grown = realloc(value, capacity);
            if (grown == NULL) {
                free(value);
                return NULL;
            }
            value = grown;
        }
        value[length++] = (char)character;
    }
    if (**cursor != '"') {
        free(value);
        return NULL;
    }
    ++*cursor;
    value[length] = '\0';
    return value;
}

static cJSON *parse_value(const char **cursor);

static bool append_child(cJSON *parent, cJSON *child)
{
    if (parent == NULL || child == NULL) return false;
    if (parent->child == NULL) {
        parent->child = child;
        return true;
    }
    cJSON *tail = parent->child;
    while (tail->next != NULL) tail = tail->next;
    tail->next = child;
    child->prev = tail;
    return true;
}

static cJSON *parse_object(const char **cursor)
{
    if (**cursor != '{') return NULL;
    ++*cursor;
    cJSON *object = new_item(cJSON_Object);
    if (object == NULL) return NULL;

    *cursor = skip_space(*cursor);
    if (**cursor == '}') {
        ++*cursor;
        return object;
    }

    while (**cursor != '\0') {
        *cursor = skip_space(*cursor);
        char *key = parse_string(cursor);
        if (key == NULL) break;
        *cursor = skip_space(*cursor);
        if (**cursor != ':') {
            free(key);
            break;
        }
        ++*cursor;
        *cursor = skip_space(*cursor);
        cJSON *value = parse_value(cursor);
        if (value == NULL) {
            free(key);
            break;
        }
        value->string = key;
        append_child(object, value);
        *cursor = skip_space(*cursor);
        if (**cursor == '}') {
            ++*cursor;
            return object;
        }
        if (**cursor != ',') break;
        ++*cursor;
    }

    cJSON_Delete(object);
    return NULL;
}

static cJSON *parse_array(const char **cursor)
{
    if (**cursor != '[') return NULL;
    ++*cursor;
    cJSON *array = new_item(cJSON_Array);
    if (array == NULL) return NULL;

    *cursor = skip_space(*cursor);
    if (**cursor == ']') {
        ++*cursor;
        return array;
    }

    while (**cursor != '\0') {
        *cursor = skip_space(*cursor);
        cJSON *value = parse_value(cursor);
        if (value == NULL) break;
        append_child(array, value);
        *cursor = skip_space(*cursor);
        if (**cursor == ']') {
            ++*cursor;
            return array;
        }
        if (**cursor != ',') break;
        ++*cursor;
    }

    cJSON_Delete(array);
    return NULL;
}

static cJSON *parse_value(const char **cursor)
{
    *cursor = skip_space(*cursor);
    if (**cursor == '{') return parse_object(cursor);
    if (**cursor == '[') return parse_array(cursor);
    if (**cursor == '"') {
        cJSON *item = new_item(cJSON_String);
        if (item == NULL) return NULL;
        item->valuestring = parse_string(cursor);
        if (item->valuestring == NULL) {
            cJSON_Delete(item);
            return NULL;
        }
        return item;
    }
    if (strncmp(*cursor, "true", 4U) == 0) {
        *cursor += 4;
        return new_item(cJSON_True);
    }
    if (strncmp(*cursor, "false", 5U) == 0) {
        *cursor += 5;
        return new_item(cJSON_False);
    }
    if (strncmp(*cursor, "null", 4U) == 0) {
        *cursor += 4;
        return new_item(cJSON_NULL);
    }

    char *end = NULL;
    const double number = strtod(*cursor, &end);
    if (end == *cursor) return NULL;
    *cursor = end;
    cJSON *item = new_item(cJSON_Number);
    if (item != NULL) {
        item->valuedouble = number;
        item->valueint = (int)number;
    }
    return item;
}

cJSON *cJSON_Parse(const char *text)
{
    if (text == NULL) return NULL;
    const char *cursor = skip_space(text);
    cJSON *root = parse_value(&cursor);
    cursor = skip_space(cursor);
    if (root == NULL || *cursor != '\0') {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

cJSON *cJSON_CreateObject(void)
{
    return new_item(cJSON_Object);
}

cJSON *cJSON_CreateNumber(double value)
{
    cJSON *item = new_item(cJSON_Number);
    if (item != NULL) {
        item->valuedouble = value;
        item->valueint = (int)value;
    }
    return item;
}

cJSON *cJSON_CreateString(const char *value)
{
    if (value == NULL) return NULL;
    cJSON *item = new_item(cJSON_String);
    if (item == NULL) return NULL;
    item->valuestring = duplicate_text(value);
    if (item->valuestring == NULL) {
        cJSON_Delete(item);
        return NULL;
    }
    return item;
}

cJSON *cJSON_CreateNull(void)
{
    return new_item(cJSON_NULL);
}

static cJSON *create_named(cJSON *object, const char *name, cJSON *value)
{
    if (object == NULL || object->type != cJSON_Object || name == NULL || value == NULL) {
        cJSON_Delete(value);
        return NULL;
    }
    value->string = duplicate_text(name);
    if (value->string == NULL || !append_child(object, value)) {
        cJSON_Delete(value);
        return NULL;
    }
    return value;
}

cJSON *cJSON_AddArrayToObject(cJSON *object, const char *name)
{
    return create_named(object, name, new_item(cJSON_Array));
}

cJSON *cJSON_AddBoolToObject(cJSON *object, const char *name, bool value)
{
    return create_named(object, name, new_item(value ? cJSON_True : cJSON_False));
}

cJSON *cJSON_AddNumberToObject(cJSON *object, const char *name, double value)
{
    return create_named(object, name, cJSON_CreateNumber(value));
}

cJSON *cJSON_AddObjectToObject(cJSON *object, const char *name)
{
    return create_named(object, name, new_item(cJSON_Object));
}

cJSON *cJSON_AddStringToObject(cJSON *object, const char *name, const char *value)
{
    cJSON *item = new_item(cJSON_String);
    if (item != NULL) {
        item->valuestring = duplicate_text(value != NULL ? value : "");
        if (item->valuestring == NULL) {
            cJSON_Delete(item);
            return NULL;
        }
    }
    return create_named(object, name, item);
}

cJSON *cJSON_AddNullToObject(cJSON *object, const char *name)
{
    return create_named(object, name, cJSON_CreateNull());
}

bool cJSON_AddItemToArray(cJSON *array, cJSON *item)
{
    return array != NULL && array->type == cJSON_Array && append_child(array, item);
}

cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *object, const char *name)
{
    if (object == NULL || object->type != cJSON_Object || name == NULL) return NULL;
    for (cJSON *item = object->child; item != NULL; item = item->next) {
        if (item->string != NULL && strcmp(item->string, name) == 0) return item;
    }
    return NULL;
}

cJSON *cJSON_GetArrayItem(const cJSON *array, int index)
{
    if (array == NULL || array->type != cJSON_Array || index < 0) return NULL;
    cJSON *item = array->child;
    while (item != NULL && index-- > 0) item = item->next;
    return item;
}

int cJSON_GetArraySize(const cJSON *array)
{
    int count = 0;
    for (const cJSON *item = array != NULL ? array->child : NULL; item != NULL; item = item->next) {
        ++count;
    }
    return count;
}

bool cJSON_IsArray(const cJSON *item) { return item != NULL && item->type == cJSON_Array; }
bool cJSON_IsNumber(const cJSON *item) { return item != NULL && item->type == cJSON_Number; }
bool cJSON_IsObject(const cJSON *item) { return item != NULL && item->type == cJSON_Object; }
bool cJSON_IsString(const cJSON *item) { return item != NULL && item->type == cJSON_String; }
bool cJSON_IsTrue(const cJSON *item) { return item != NULL && item->type == cJSON_True; }

bool cJSON_IsBool(const cJSON *item) { return item != NULL && (item->type == cJSON_True || item->type == cJSON_False); }
bool cJSON_IsNull(const cJSON *item) { return item != NULL && item->type == cJSON_NULL; }

void cJSON_DeleteItemFromObjectCaseSensitive(cJSON *object, const char *name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (item == NULL) return;
    if (item->prev != NULL) item->prev->next = item->next;
    else object->child = item->next;
    if (item->next != NULL) item->next->prev = item->prev;
    item->next = NULL;
    item->prev = NULL;
    delete_single(item);
}

typedef struct
{
    char *data;
    size_t length;
    size_t capacity;
} writer_t;

static bool writer_append(writer_t *writer, const char *text)
{
    const size_t length = strlen(text);
    if (writer->length + length + 1U > writer->capacity) {
        size_t capacity = writer->capacity == 0U ? 128U : writer->capacity;
        while (capacity < writer->length + length + 1U) capacity *= 2U;
        char *grown = realloc(writer->data, capacity);
        if (grown == NULL) return false;
        writer->data = grown;
        writer->capacity = capacity;
    }
    memcpy(writer->data + writer->length, text, length);
    writer->length += length;
    writer->data[writer->length] = '\0';
    return true;
}

static bool writer_append_string(writer_t *writer, const char *value)
{
    if (!writer_append(writer, "\"")) return false;
    for (const char *cursor = value != NULL ? value : ""; *cursor != '\0'; ++cursor) {
        char escaped[3] = {0};
        const char character = *cursor;
        if (character == '"' || character == '\\') {
            escaped[0] = '\\';
            escaped[1] = character;
        } else if (character == '\n') {
            escaped[0] = '\\';
            escaped[1] = 'n';
        } else {
            escaped[0] = character;
        }
        if (!writer_append(writer, escaped)) return false;
    }
    return writer_append(writer, "\"");
}

static bool print_value(writer_t *writer, const cJSON *item)
{
    if (item == NULL) return false;
    if (item->type == cJSON_Object || item->type == cJSON_Array) {
        const bool object = item->type == cJSON_Object;
        if (!writer_append(writer, object ? "{" : "[")) return false;
        bool first = true;
        for (const cJSON *child = item->child; child != NULL; child = child->next) {
            if (!first && !writer_append(writer, ",")) return false;
            first = false;
            if (object) {
                if (!writer_append_string(writer, child->string) || !writer_append(writer, ":")) return false;
            }
            if (!print_value(writer, child)) return false;
        }
        return writer_append(writer, object ? "}" : "]");
    }
    if (item->type == cJSON_String) return writer_append_string(writer, item->valuestring);
    if (item->type == cJSON_True) return writer_append(writer, "true");
    if (item->type == cJSON_False) return writer_append(writer, "false");
    if (item->type == cJSON_NULL) return writer_append(writer, "null");

    char number[64];
    snprintf(number, sizeof(number), "%.17g", item->valuedouble);
    return writer_append(writer, number);
}

char *cJSON_PrintUnformatted(const cJSON *item)
{
    writer_t writer = {0};
    if (!print_value(&writer, item)) {
        free(writer.data);
        return NULL;
    }
    return writer.data;
}

void cJSON_free(void *pointer)
{
    free(pointer);
}
