#undef malloc
#undef calloc
#undef realloc
#undef free

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <stdlib.h>
#endif

typedef union
{
    long double alignment;
    struct
    {
        size_t size;
        uint64_t magic;
    } value;
} max132_allocation_header_t;

#define MAX132_ALLOCATION_MAGIC UINT64_C(0x4D4158313332414C)
#define MAX132_METHOD_SLOTS 32U
#define MAX132_METHOD_NAME_SIZE 64U

typedef struct
{
    char name[MAX132_METHOD_NAME_SIZE];
    size_t attempts;
} max132_method_allocations_t;

static bool s_active;
static bool s_failed;
static size_t s_attempts;
static size_t s_fail_at;
static size_t s_live_blocks;
static size_t s_live_bytes;
static bool s_fail_method_only;
static char s_fail_method[MAX132_METHOD_NAME_SIZE];
static char s_current_method[MAX132_METHOD_NAME_SIZE];
static max132_method_allocations_t s_method_allocations[MAX132_METHOD_SLOTS];

void max132_test_free(void *pointer);

static max132_method_allocations_t *method_slot(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0U; i < MAX132_METHOD_SLOTS; ++i) {
        if (s_method_allocations[i].name[0] == '\0') {
            (void)snprintf(s_method_allocations[i].name,
                sizeof(s_method_allocations[i].name), "%s", name);
            return &s_method_allocations[i];
        }
        if (strcmp(s_method_allocations[i].name, name) == 0) {
            return &s_method_allocations[i];
        }
    }
    return NULL;
}

static bool reject_this_allocation(void)
{
    if (!s_active) {
        return false;
    }
    ++s_attempts;
    max132_method_allocations_t *slot = method_slot(s_current_method);
    if (slot != NULL) {
        ++slot->attempts;
    }
    if (s_fail_method_only) {
        if (s_current_method[0] != '\0' && strcmp(s_current_method, s_fail_method) == 0) {
            s_failed = true;
            return true;
        }
        return false;
    }
    if (s_fail_at != 0U && s_attempts == s_fail_at) {
        s_failed = true;
        return true;
    }
    return false;
}

static void *allocate_block(size_t size, bool clear)
{
    if (reject_this_allocation() || size > SIZE_MAX - sizeof(max132_allocation_header_t)) {
        return NULL;
    }
    const size_t payload_size = size == 0U ? 1U : size;
#ifdef _WIN32
    max132_allocation_header_t *header = HeapAlloc(
        GetProcessHeap(), 0U, sizeof(*header) + payload_size
    );
#else
    max132_allocation_header_t *header = malloc(sizeof(*header) + payload_size);
#endif
    if (header == NULL) {
        return NULL;
    }
    header->value.size = payload_size;
    header->value.magic = MAX132_ALLOCATION_MAGIC;
    ++s_live_blocks;
    s_live_bytes += payload_size;
    void *payload = header + 1;
    if (clear) {
        memset(payload, 0, payload_size);
    }
    return payload;
}

void *max132_test_malloc(size_t size)
{
    return allocate_block(size, false);
}

void *max132_test_calloc(size_t count, size_t size)
{
    if (count != 0U && size > SIZE_MAX / count) {
        (void)reject_this_allocation();
        return NULL;
    }
    return allocate_block(count * size, true);
}

void *max132_test_realloc(void *pointer, size_t size)
{
    if (pointer == NULL) {
        return max132_test_malloc(size);
    }
    if (size == 0U) {
        max132_test_free(pointer);
        return NULL;
    }
    max132_allocation_header_t *old_header = (max132_allocation_header_t *)pointer - 1;
    assert(old_header->value.magic == MAX132_ALLOCATION_MAGIC);
    void *new_pointer = allocate_block(size, false);
    if (new_pointer == NULL) {
        return NULL;
    }
    const size_t copy_size = old_header->value.size < size ? old_header->value.size : size;
    memcpy(new_pointer, pointer, copy_size);
    max132_test_free(pointer);
    return new_pointer;
}

void max132_test_free(void *pointer)
{
    if (pointer == NULL) {
        return;
    }
    max132_allocation_header_t *header = (max132_allocation_header_t *)pointer - 1;
    assert(header->value.magic == MAX132_ALLOCATION_MAGIC);
    assert(s_live_blocks > 0U && s_live_bytes >= header->value.size);
    --s_live_blocks;
    s_live_bytes -= header->value.size;
    header->value.magic = 0U;
#ifdef _WIN32
    assert(HeapFree(GetProcessHeap(), 0U, header) != 0);
#else
    free(header);
#endif
}

void max132_fault_allocator_start(size_t fail_at)
{
    memset(s_method_allocations, 0, sizeof(s_method_allocations));
    s_attempts = 0U;
    s_fail_at = fail_at;
    s_failed = false;
    s_fail_method_only = false;
    s_fail_method[0] = '\0';
    s_current_method[0] = '\0';
    s_active = true;
}

void max132_fault_allocator_start_method(const char *method_name)
{
    assert(method_name != NULL && method_name[0] != '\0');
    max132_fault_allocator_start(0U);
    s_fail_method_only = true;
    (void)snprintf(s_fail_method, sizeof(s_fail_method), "%s", method_name);
}

void max132_fault_allocator_stop(void)
{
    s_active = false;
    s_fail_method_only = false;
    s_current_method[0] = '\0';
}

void max132_fault_allocator_set_method(const char *method_name)
{
    (void)snprintf(s_current_method, sizeof(s_current_method), "%s",
        method_name == NULL ? "" : method_name);
}

size_t max132_fault_allocator_attempts(void) { return s_attempts; }
size_t max132_fault_allocator_live_blocks(void) { return s_live_blocks; }
size_t max132_fault_allocator_live_bytes(void) { return s_live_bytes; }
bool max132_fault_allocator_hit(void) { return s_failed; }

size_t max132_fault_allocator_method_attempts(const char *method_name)
{
    max132_method_allocations_t *slot = method_slot(method_name);
    return slot == NULL ? 0U : slot->attempts;
}
