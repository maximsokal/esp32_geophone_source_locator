#include "localization_event_rank.h"

#include <float.h>
#include <math.h>
#include <string.h>

enum { RANK_KEY_BYTES = 20, RANK_PAGE_ITEMS = 32, RANK_BUCKETS = 256 };

_Static_assert(sizeof(float) == sizeof(uint32_t), "rank requires 32-bit float");
_Static_assert(FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128,
               "rank requires binary32 float");

static bool rank_key(const localization_event_rank_item_t *item, uint64_t ordinal,
                     uint8_t key[RANK_KEY_BYTES])
{
    if (!isfinite(item->peak_abs) || item->peak_abs < 0.0f) return false;
    float peak = item->peak_abs == 0.0f ? 0.0f : item->peak_abs;
    uint32_t bits;
    memcpy(&bits, &peak, sizeof(bits));
    bits = ~bits;
    for (size_t i = 0U; i < 4U; ++i) key[i] = (uint8_t)(bits >> (24U - 8U * i));
    for (size_t i = 0U; i < 8U; ++i) {
        key[4U + i] = (uint8_t)(item->source_index >> (56U - 8U * i));
        key[12U + i] = (uint8_t)(ordinal >> (56U - 8U * i));
    }
    return true;
}

esp_err_t localization_event_rank_select(
    uint64_t count, unsigned percent, localization_event_rank_read_fn read,
    void *context, localization_event_rank_cutoff_t *cutoff
)
{
    if (cutoff == NULL || read == NULL || percent > 100U) return ESP_ERR_INVALID_ARG;
    *cutoff = (localization_event_rank_cutoff_t) { 0 };
    if (count == 0U || percent == 0U) return ESP_OK;

    const float one = 1.0f;
    uint32_t one_bits;
    memcpy(&one_bits, &one, sizeof(one_bits));
    if (one_bits != UINT32_C(0x3f800000)) return ESP_ERR_INVALID_STATE;

    const uint64_t whole = count / 100U;
    const uint64_t remainder = count % 100U;
    const uint64_t selected = whole * percent + (remainder * percent + 99U) / 100U;
    uint64_t rank = selected - 1U;
    uint8_t prefix[RANK_KEY_BYTES] = {0};
    localization_event_rank_item_t page[RANK_PAGE_ITEMS];

    for (size_t digit = 0U; digit < RANK_KEY_BYTES; ++digit) {
        uint64_t histogram[RANK_BUCKETS] = {0};
        uint64_t offset = 0U;
        while (offset < count) {
            const uint64_t remaining = count - offset;
            const size_t request = remaining < RANK_PAGE_ITEMS ? (size_t)remaining : RANK_PAGE_ITEMS;
            size_t received = 0U;
            const esp_err_t err = read(context, offset, page, request, &received);
            if (err != ESP_OK) return err;
            if (received == 0U || received > request) return ESP_ERR_INVALID_RESPONSE;
            for (size_t i = 0U; i < received; ++i) {
                uint8_t key[RANK_KEY_BYTES];
                if (!rank_key(&page[i], offset + (uint64_t)i, key)) return ESP_ERR_INVALID_RESPONSE;
                if (memcmp(key, prefix, digit) == 0) ++histogram[key[digit]];
            }
            offset += (uint64_t)received;
        }
        size_t bucket = 0U;
        while (bucket < RANK_BUCKETS && rank >= histogram[bucket]) {
            rank -= histogram[bucket];
            ++bucket;
        }
        if (bucket == RANK_BUCKETS) return ESP_ERR_INVALID_RESPONSE;
        prefix[digit] = (uint8_t)bucket;
    }
    memcpy(cutoff->key, prefix, sizeof(prefix));
    cutoff->selected_count = selected;
    cutoff->has_cutoff = true;
    return ESP_OK;
}

bool localization_event_rank_included(
    const localization_event_rank_cutoff_t *cutoff,
    const localization_event_rank_item_t *item, uint64_t ordinal
)
{
    if (cutoff == NULL || item == NULL || !cutoff->has_cutoff) return false;
    uint8_t key[RANK_KEY_BYTES];
    return rank_key(item, ordinal, key) && memcmp(key, cutoff->key, sizeof(key)) <= 0;
}
