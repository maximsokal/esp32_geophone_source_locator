#pragma once

#include <stdbool.h>

#ifdef _MSC_VER
#pragma warning(disable: 4244)
#pragma warning(disable: 4189)
#endif

typedef struct cJSON
{
    struct cJSON *next;
    struct cJSON *prev;
    struct cJSON *child;
    char *string;
    int type;
    int valueint;
    double valuedouble;
    char *valuestring;
} cJSON;

#define cJSON_False  (0)
#define cJSON_True   (1)
#define cJSON_NULL   (1 << 1)
#define cJSON_Number (1 << 3)
#define cJSON_String (1 << 4)
#define cJSON_Array  (1 << 5)
#define cJSON_Object (1 << 6)

cJSON *cJSON_Parse(const char *text);
cJSON *cJSON_CreateObject(void);
cJSON *cJSON_CreateNumber(double value);
cJSON *cJSON_CreateString(const char *value);
cJSON *cJSON_CreateNull(void);
cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *object, const char *name);
cJSON *cJSON_GetArrayItem(const cJSON *array, int index);
int cJSON_GetArraySize(const cJSON *array);
cJSON *cJSON_AddArrayToObject(cJSON *object, const char *name);
cJSON *cJSON_AddBoolToObject(cJSON *object, const char *name, bool value);
cJSON *cJSON_AddNumberToObject(cJSON *object, const char *name, double value);
cJSON *cJSON_AddObjectToObject(cJSON *object, const char *name);
cJSON *cJSON_AddStringToObject(cJSON *object, const char *name, const char *value);
cJSON *cJSON_AddNullToObject(cJSON *object, const char *name);
bool cJSON_AddItemToArray(cJSON *array, cJSON *item);
bool cJSON_IsArray(const cJSON *item);
bool cJSON_IsNumber(const cJSON *item);
bool cJSON_IsObject(const cJSON *item);
bool cJSON_IsString(const cJSON *item);
bool cJSON_IsTrue(const cJSON *item);
bool cJSON_IsBool(const cJSON *item);
bool cJSON_IsNull(const cJSON *item);
void cJSON_DeleteItemFromObjectCaseSensitive(cJSON *object, const char *name);
void cJSON_Delete(cJSON *item);
char *cJSON_PrintUnformatted(const cJSON *item);
void cJSON_free(void *pointer);
