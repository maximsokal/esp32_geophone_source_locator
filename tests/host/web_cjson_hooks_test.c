#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "web_cjson_hooks.h"

typedef struct {
    void *result;
    size_t calls;
    size_t last_size;
} fake_allocator_t;

static fake_allocator_t s_psram;
static fake_allocator_t s_internal;
static unsigned char s_psram_block;
static unsigned char s_internal_block;

static void *fake_psram_alloc(size_t size)
{
    ++s_psram.calls;
    s_psram.last_size = size;
    return s_psram.result;
}

static void *fake_internal_alloc(size_t size)
{
    ++s_internal.calls;
    s_internal.last_size = size;
    return s_internal.result;
}

static void reset_fakes(void)
{
    s_psram = (fake_allocator_t){0};
    s_internal = (fake_allocator_t){0};
}

#define CHECK(case_name, expression) \
    do { \
        if (!(expression)) { \
            fprintf(stderr, "web_cjson_hooks_test: %s failed\n", case_name); \
            return EXIT_FAILURE; \
        } \
    } while (0)

int main(void)
{
    reset_fakes();
    s_psram.result = &s_psram_block;
    s_internal.result = &s_internal_block;
    CHECK(
        "PSRAM success and size forwarding",
        web_cjson_alloc_with_fallback(37U, fake_psram_alloc, fake_internal_alloc) ==
            &s_psram_block &&
        s_psram.calls == 1U && s_psram.last_size == 37U &&
        s_internal.calls == 0U
    );

    reset_fakes();
    s_internal.result = &s_internal_block;
    CHECK(
        "internal fallback and size forwarding",
        web_cjson_alloc_with_fallback(73U, fake_psram_alloc, fake_internal_alloc) ==
            &s_internal_block &&
        s_psram.calls == 1U && s_psram.last_size == 73U &&
        s_internal.calls == 1U && s_internal.last_size == 73U
    );

    reset_fakes();
    CHECK(
        "both allocators fail",
        web_cjson_alloc_with_fallback(51U, fake_psram_alloc, fake_internal_alloc) ==
            NULL &&
        s_psram.calls == 1U && s_internal.calls == 1U
    );

    reset_fakes();
    s_internal.result = &s_internal_block;
    CHECK(
        "zero size pass-through",
        web_cjson_alloc_with_fallback(0U, fake_psram_alloc, fake_internal_alloc) ==
            &s_internal_block &&
        s_psram.calls == 1U && s_psram.last_size == 0U &&
        s_internal.calls == 1U && s_internal.last_size == 0U
    );

    reset_fakes();
    s_internal.result = &s_internal_block;
    CHECK(
        "NULL PSRAM function",
        web_cjson_alloc_with_fallback(29U, NULL, fake_internal_alloc) ==
            &s_internal_block &&
        s_psram.calls == 0U && s_internal.calls == 1U &&
        s_internal.last_size == 29U
    );

    reset_fakes();
    s_psram.result = &s_psram_block;
    CHECK(
        "NULL internal function",
        web_cjson_alloc_with_fallback(41U, fake_psram_alloc, NULL) ==
            &s_psram_block &&
        s_psram.calls == 1U && s_psram.last_size == 41U &&
        s_internal.calls == 0U
    );

    reset_fakes();
    CHECK(
        "NULL internal function after PSRAM failure",
        web_cjson_alloc_with_fallback(43U, fake_psram_alloc, NULL) == NULL &&
        s_psram.calls == 1U && s_psram.last_size == 43U &&
        s_internal.calls == 0U
    );

    CHECK(
        "both functions NULL",
        web_cjson_alloc_with_fallback(13U, NULL, NULL) == NULL
    );

    web_cjson_install_psram_hooks();
    web_cjson_install_psram_hooks();
    puts("web_cjson_hooks_test: PASS");
    return EXIT_SUCCESS;
}
