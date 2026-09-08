#pragma once

#include <stdbool.h>

typedef struct cJSON
{
    struct cJSON *next;
    char *string;
    int type;
    int valueint;
    char *valuestring;
} cJSON;

#define cJSON_Number  (1 << 3)
#define cJSON_String  (1 << 4)

cJSON *cJSON_Parse(const char *text);
cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *object, const char *name);
bool cJSON_IsNumber(const cJSON *item);
bool cJSON_IsString(const cJSON *item);
void cJSON_Delete(cJSON *item);
