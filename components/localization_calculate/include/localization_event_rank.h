#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    float peak_abs;
    uint64_t source_index;
} localization_event_rank_item_t;

/* The reader must expose the same immutable ordered records on every pass. */
typedef esp_err_t (*localization_event_rank_read_fn)(
    void *context, uint64_t start, localization_event_rank_item_t *items,
    size_t capacity, size_t *count
);

typedef struct {
    uint64_t selected_count;
    bool has_cutoff;
    uint8_t key[20];
} localization_event_rank_cutoff_t;

esp_err_t localization_event_rank_select(
    uint64_t count, unsigned percent, localization_event_rank_read_fn read,
    void *context, localization_event_rank_cutoff_t *cutoff
);

bool localization_event_rank_included(
    const localization_event_rank_cutoff_t *cutoff,
    const localization_event_rank_item_t *item, uint64_t ordinal
);
