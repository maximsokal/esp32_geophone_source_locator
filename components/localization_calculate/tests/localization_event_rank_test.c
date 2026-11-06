#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "localization_event_rank.h"

typedef struct {
    localization_event_rank_item_t *items;
    size_t count;
    size_t reads;
    size_t max_request;
    bool fail;
    bool short_page;
    bool empty_page;
} fixture_t;

static esp_err_t read_items(void *context, uint64_t start, localization_event_rank_item_t *out,
                            size_t capacity, size_t *count)
{
    fixture_t *fixture = context;
    if (fixture->fail) return ESP_FAIL;
    assert(start <= fixture->count);
    if (capacity > fixture->max_request) fixture->max_request = capacity;
    size_t remaining = fixture->count - (size_t)start;
    *count = remaining < capacity ? remaining : capacity;
    if (fixture->short_page && *count > 1U) *count = 1U;
    if (fixture->empty_page) *count = 0U;
    for (size_t i = 0U; i < *count; ++i) out[i] = fixture->items[(size_t)start + i];
    ++fixture->reads;
    return ESP_OK;
}

typedef struct {
    localization_event_rank_item_t item;
    size_t ordinal;
} expected_t;

static int compare_expected(const void *left, const void *right)
{
    const expected_t *a = left;
    const expected_t *b = right;
    if (a->item.peak_abs != b->item.peak_abs) return a->item.peak_abs > b->item.peak_abs ? -1 : 1;
    if (a->item.source_index != b->item.source_index) return a->item.source_index < b->item.source_index ? -1 : 1;
    if (a->ordinal != b->ordinal) return a->ordinal < b->ordinal ? -1 : 1;
    return 0;
}

static void test_exact_top_percent_with_ties_and_paged_input(void)
{
    enum { count = 301 };
    localization_event_rank_item_t items[count];
    expected_t expected[count];
    for (size_t i = 0U; i < count; ++i) {
        items[i] = (localization_event_rank_item_t) {
            .peak_abs = (float)((i * 37U) % 11U),
            .source_index = (uint64_t)((i * 13U) % 19U)
        };
        expected[i] = (expected_t) { .item = items[i], .ordinal = i };
    }
    qsort(expected, count, sizeof(expected[0]), compare_expected);
    fixture_t fixture = { .items = items, .count = count };
    localization_event_rank_cutoff_t cutoff;
    assert(localization_event_rank_select(count, 5U, read_items, &fixture, &cutoff) == ESP_OK);
    assert(cutoff.selected_count == 16U && cutoff.has_cutoff);
    assert(fixture.reads > 1U && fixture.max_request < count);
    for (size_t i = 0U; i < count; ++i) {
        const bool selected = localization_event_rank_included(&cutoff, &expected[i].item, expected[i].ordinal);
        assert(selected == (i < 16U));
    }
}

static void test_empty_all_invalid_and_read_failure(void)
{
    localization_event_rank_item_t items[] = {
        { .peak_abs = 1.0f, .source_index = 12U },
        { .peak_abs = 1.0f, .source_index = 3U },
        { .peak_abs = 0.0f, .source_index = 1U }
    };
    fixture_t fixture = { .items = items, .count = 3U };
    localization_event_rank_cutoff_t cutoff;
    assert(localization_event_rank_select(3U, 0U, read_items, &fixture, &cutoff) == ESP_OK);
    assert(!cutoff.has_cutoff && cutoff.selected_count == 0U && fixture.reads == 0U);
    assert(localization_event_rank_select(0U, 5U, read_items, &fixture, &cutoff) == ESP_OK);
    assert(!cutoff.has_cutoff && cutoff.selected_count == 0U);
    assert(localization_event_rank_select(3U, 100U, read_items, &fixture, &cutoff) == ESP_OK);
    assert(cutoff.selected_count == 3U);
    for (size_t i = 0U; i < 3U; ++i) assert(localization_event_rank_included(&cutoff, &items[i], i));
    assert(localization_event_rank_select(3U, 101U, read_items, &fixture, &cutoff) == ESP_ERR_INVALID_ARG);
    fixture.short_page = true;
    assert(localization_event_rank_select(3U, 5U, read_items, &fixture, &cutoff) == ESP_OK);
    assert(localization_event_rank_included(&cutoff, &items[1], 1U));
    assert(!localization_event_rank_included(&cutoff, &items[0], 0U));
    fixture.short_page = false;
    fixture.empty_page = true;
    assert(localization_event_rank_select(3U, 5U, read_items, &fixture, &cutoff) == ESP_ERR_INVALID_RESPONSE);
    fixture.empty_page = false;
    items[1].peak_abs = NAN;
    assert(localization_event_rank_select(3U, 5U, read_items, &fixture, &cutoff) == ESP_ERR_INVALID_RESPONSE);
    items[1].peak_abs = -1.0f;
    assert(localization_event_rank_select(3U, 5U, read_items, &fixture, &cutoff) == ESP_ERR_INVALID_RESPONSE);
    items[1].peak_abs = 1.0f;
    fixture.fail = true;
    assert(localization_event_rank_select(3U, 5U, read_items, &fixture, &cutoff) == ESP_FAIL);
}

int main(void)
{
    test_exact_top_percent_with_ties_and_paged_input();
    test_empty_all_invalid_and_read_failure();
    return 0;
}
