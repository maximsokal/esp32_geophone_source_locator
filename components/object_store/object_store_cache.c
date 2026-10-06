#include "object_store_internal.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif

#define OBJECT_STORE_CALC_CACHE_BYTES \
    ((size_t)OBJECT_STORE_MAX_OBJECTS * OBJECT_STORE_CALC_SETTINGS_JSON_SIZE)

typedef struct {
    bool used;
    char object_id[OBJECT_STORE_ID_SIZE];
    uint64_t generation;
    object_store_cache_calc_state_t calc_state;
} object_store_cache_entry_t;

static object_store_cache_entry_t s_cache_entries[OBJECT_STORE_MAX_OBJECTS];
static char *s_calc_cache_bytes;
static uint64_t s_generation_sequence = 1U;

static uint64_t object_store_cache_next_generation(void)
{
    ++s_generation_sequence;
    if (s_generation_sequence == 0U) {
        s_generation_sequence = 1U;
    }
    return s_generation_sequence;
}

static int object_store_cache_find(const char *object_id)
{
    for (size_t i = 0U; i < OBJECT_STORE_MAX_OBJECTS; ++i) {
        if (s_cache_entries[i].used &&
            strcmp(s_cache_entries[i].object_id, object_id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int object_store_cache_allocate(const char *object_id)
{
    int available = -1;
    for (size_t i = 0U; i < OBJECT_STORE_MAX_OBJECTS; ++i) {
        if (!s_cache_entries[i].used) {
            available = (int)i;
            break;
        }
    }

    if (available < 0) {
        object_store_cache_reset();
        available = 0;
    }

    object_store_cache_entry_t *entry = &s_cache_entries[(size_t)available];
    memset(entry, 0, sizeof(*entry));
    entry->used = true;
    (void)snprintf(entry->object_id, sizeof(entry->object_id), "%s", object_id);
    entry->generation = object_store_cache_next_generation();
    return available;
}

static bool object_store_cache_calc_storage_ensure(void)
{
    if (s_calc_cache_bytes != NULL) {
        return true;
    }

#if defined(ESP_PLATFORM)
    s_calc_cache_bytes = heap_caps_malloc(
        OBJECT_STORE_CALC_CACHE_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (s_calc_cache_bytes == NULL) {
        s_calc_cache_bytes = heap_caps_malloc(
            OBJECT_STORE_CALC_CACHE_BYTES,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        );
    }
#else
    s_calc_cache_bytes = malloc(OBJECT_STORE_CALC_CACHE_BYTES);
#endif

    if (s_calc_cache_bytes != NULL) {
        memset(s_calc_cache_bytes, 0, OBJECT_STORE_CALC_CACHE_BYTES);
    }
    return s_calc_cache_bytes != NULL;
}

void object_store_cache_reset(void)
{
    memset(s_cache_entries, 0, sizeof(s_cache_entries));
    (void)object_store_cache_next_generation();
}

void object_store_cache_generation_bump_all(void)
{
    object_store_cache_reset();
}

bool object_store_cache_generation_get(const char *object_id, uint64_t *generation_out)
{
    if (object_id == NULL || generation_out == NULL) {
        return false;
    }

    int index = object_store_cache_find(object_id);
    if (index < 0) {
        index = object_store_cache_allocate(object_id);
    }
    *generation_out = s_cache_entries[(size_t)index].generation;
    return true;
}

bool object_store_cache_generation_matches(const char *object_id, uint64_t generation)
{
    if (object_id == NULL || generation == 0U) {
        return false;
    }
    const int index = object_store_cache_find(object_id);
    return index >= 0 && s_cache_entries[(size_t)index].generation == generation;
}

bool object_store_cache_generation_bump(const char *object_id, uint64_t *generation_out)
{
    if (object_id == NULL) {
        return false;
    }

    int index = object_store_cache_find(object_id);
    if (index < 0) {
        index = object_store_cache_allocate(object_id);
    }
    object_store_cache_entry_t *entry = &s_cache_entries[(size_t)index];
    entry->generation = object_store_cache_next_generation();
    entry->calc_state = OBJECT_STORE_CACHE_CALC_MISS;
    if (generation_out != NULL) {
        *generation_out = entry->generation;
    }
    return true;
}

object_store_cache_calc_state_t object_store_cache_calc_state(
    const char *object_id,
    uint64_t generation
)
{
    if (!object_store_cache_generation_matches(object_id, generation)) {
        return OBJECT_STORE_CACHE_CALC_MISS;
    }
    const int index = object_store_cache_find(object_id);
    return s_cache_entries[(size_t)index].calc_state;
}

bool object_store_cache_calc_copy(
    const char *object_id,
    uint64_t generation,
    char *buffer,
    size_t buffer_size
)
{
    if (buffer == NULL || buffer_size == 0U ||
        object_store_cache_calc_state(object_id, generation) !=
            OBJECT_STORE_CACHE_CALC_PRESENT ||
        s_calc_cache_bytes == NULL) {
        return false;
    }

    const int index = object_store_cache_find(object_id);
    const char *cached = s_calc_cache_bytes +
        (size_t)index * OBJECT_STORE_CALC_SETTINGS_JSON_SIZE;
    const size_t length = strlen(cached);
    if (length + 1U > buffer_size) {
        return false;
    }
    memcpy(buffer, cached, length + 1U);
    return true;
}

void object_store_cache_calc_store(
    const char *object_id,
    uint64_t generation,
    const char *json,
    bool present
)
{
    if (!object_store_cache_generation_matches(object_id, generation)) {
        return;
    }

    const int index = object_store_cache_find(object_id);
    object_store_cache_entry_t *entry = &s_cache_entries[(size_t)index];
    if (!present) {
        entry->calc_state = OBJECT_STORE_CACHE_CALC_ABSENT;
        return;
    }
    if (json == NULL || !object_store_cache_calc_storage_ensure()) {
        return;
    }

    char *cached = s_calc_cache_bytes +
        (size_t)index * OBJECT_STORE_CALC_SETTINGS_JSON_SIZE;
    const size_t length = strlen(json);
    if (length == 0U || length >= OBJECT_STORE_CALC_SETTINGS_JSON_SIZE) {
        return;
    }
    memcpy(cached, json, length + 1U);
    entry->calc_state = OBJECT_STORE_CACHE_CALC_PRESENT;
}
