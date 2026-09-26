#include "scan_preview_state.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    unsigned allocations;
    unsigned frees;
} allocation_counts_t;

static void *count_alloc(size_t size, void *context)
{
    allocation_counts_t *counts = context;
    void *allocation = malloc(size);
    if (allocation != NULL) counts->allocations++;
    return allocation;
}

static void count_free(void *allocation, void *context)
{
    allocation_counts_t *counts = context;
    if (allocation != NULL) counts->frees++;
    free(allocation);
}

static scan_preview_t allocated_preview(allocation_counts_t *counts)
{
    scan_preview_t preview = {0};
    preview.allocator = (scan_preview_allocator_t){count_alloc, count_free, counts};
    preview.allocation = count_alloc(sizeof(uint32_t) + 1U, counts);
    assert(preview.allocation != NULL);
    preview.indices = preview.allocation;
    preview.face_masks = (uint8_t *)preview.allocation + sizeof(uint32_t);
    preview.voxel_count = 1U;
    return preview;
}

static void test_session_switch_discards_old_load(void)
{
    scan_preview_state_t state;
    allocation_counts_t counts = {0};
    scan_preview_state_init(&state);
    const uint32_t session_a = scan_preview_state_open(&state);
    scan_preview_t old_preview = allocated_preview(&counts);
    const uint32_t session_b = scan_preview_state_open(&state);
    assert(session_a != session_b);
    assert(!scan_preview_state_complete(&state, session_a, SCAN_PREVIEW_SCREEN_READY, &old_preview, NULL, NULL));
    assert(old_preview.allocation == NULL && counts.allocations == counts.frees);
    assert(state.status == SCAN_PREVIEW_SCREEN_LOADING && !state.has_preview);
    assert(scan_preview_state_complete(&state, session_b, SCAN_PREVIEW_SCREEN_NO_3D_SCAN, NULL, NULL, NULL));
    assert(state.status == SCAN_PREVIEW_SCREEN_NO_3D_SCAN);
    scan_preview_state_close(&state);
}

static void test_no_preview_and_render_gate(void)
{
    scan_preview_state_t state;
    allocation_counts_t counts = {0};
    scan_preview_state_init(&state);
    const uint32_t generation = scan_preview_state_open(&state);
    assert(scan_preview_state_complete(&state, generation, SCAN_PREVIEW_SCREEN_NO_PREVIEW, NULL, NULL, "stale source CRC"));
    assert(state.status == SCAN_PREVIEW_SCREEN_NO_PREVIEW);
    assert(strcmp(state.fallback_reason, "stale source CRC") == 0);
    assert(!scan_preview_state_render_begin(&state, generation));

    scan_preview_t preview = allocated_preview(&counts);
    assert(scan_preview_state_complete(&state, generation, SCAN_PREVIEW_SCREEN_READY, &preview, NULL, NULL));
    assert(preview.allocation == NULL && state.has_preview);
    assert(scan_preview_state_render_begin(&state, generation));
    assert(!scan_preview_state_render_begin(&state, generation));
    scan_preview_state_render_end(&state, generation);
    assert(scan_preview_state_render_begin(&state, generation));
    scan_preview_state_render_end(&state, generation);
    scan_preview_state_close(&state);
    assert(counts.allocations == counts.frees);
}

static void test_back_drops_pending_load_and_render_generation(void)
{
    scan_preview_state_t state;
    allocation_counts_t counts = {0};
    scan_preview_state_init(&state);
    const uint32_t old_generation = scan_preview_state_open(&state);
    scan_preview_state_close(&state); /* LEFT Back invalidates the active screen generation. */
    assert(!state.open && state.generation != old_generation);

    scan_preview_t old_load = allocated_preview(&counts);
    assert(!scan_preview_state_complete(&state, old_generation, SCAN_PREVIEW_SCREEN_READY, &old_load, NULL, NULL));
    assert(old_load.allocation == NULL && counts.allocations == counts.frees);

    const uint32_t current_generation = scan_preview_state_open(&state);
    scan_preview_t current_load = allocated_preview(&counts);
    assert(scan_preview_state_complete(&state, current_generation, SCAN_PREVIEW_SCREEN_READY, &current_load, NULL, NULL));
    assert(scan_preview_state_render_begin(&state, current_generation));
    scan_preview_state_close(&state); /* Back also invalidates a render lease. */
    assert(!state.render_in_flight);
    assert(!scan_preview_state_render_begin(&state, current_generation));

    const uint32_t next_generation = scan_preview_state_open(&state);
    scan_preview_t next_load = allocated_preview(&counts);
    assert(scan_preview_state_complete(&state, next_generation, SCAN_PREVIEW_SCREEN_READY, &next_load, NULL, NULL));
    assert(!scan_preview_state_render_begin(&state, current_generation));
    assert(scan_preview_state_render_begin(&state, next_generation));
    scan_preview_state_render_end(&state, current_generation);
    assert(state.render_in_flight); /* A stale render end cannot release the new generation. */
    scan_preview_state_render_end(&state, next_generation);
    scan_preview_state_close(&state);
    assert(counts.allocations == counts.frees);
}

static void test_one_thousand_open_close_cycles_do_not_leak(void)
{
    scan_preview_state_t state;
    allocation_counts_t counts = {0};
    scan_preview_state_init(&state);
    for (unsigned i = 0U; i < 1000U; ++i) {
        const uint32_t generation = scan_preview_state_open(&state);
        scan_preview_t preview = allocated_preview(&counts);
        assert(scan_preview_state_complete(&state, generation, SCAN_PREVIEW_SCREEN_READY, &preview, NULL, NULL));
        scan_preview_state_close(&state);
    }
    assert(counts.allocations == 1000U && counts.frees == 1000U);
}

int main(void)
{
    test_session_switch_discards_old_load();
    test_no_preview_and_render_gate();
    test_back_drops_pending_load_and_render_generation();
    test_one_thousand_open_close_cycles_do_not_leak();
    return 0;
}
