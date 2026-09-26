#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scan_preview.h"

static const uint8_t s_golden[] = {
#include "preview_v1_golden_bytes.inc"
};

typedef struct
{
    size_t allocations;
    size_t frees;
    size_t outstanding;
    bool fail;
} counting_allocator_t;

static void *counting_alloc(size_t bytes, void *context)
{
    counting_allocator_t *counter = context;
    assert(counter != NULL);
    if (counter->fail) return NULL;
    void *allocation = malloc(bytes);
    if (allocation != NULL) {
        counter->allocations++;
        counter->outstanding++;
    }
    return allocation;
}

static void counting_free(void *allocation, void *context)
{
    counting_allocator_t *counter = context;
    assert(counter != NULL && allocation != NULL && counter->outstanding > 0U);
    counter->frees++;
    counter->outstanding--;
    free(allocation);
}

static void write_u16(uint8_t *bytes, size_t offset, uint16_t value)
{
    bytes[offset] = (uint8_t)value;
    bytes[offset + 1U] = (uint8_t)(value >> 8U);
}

static void write_u32(uint8_t *bytes, size_t offset, uint32_t value)
{
    bytes[offset] = (uint8_t)value;
    bytes[offset + 1U] = (uint8_t)(value >> 8U);
    bytes[offset + 2U] = (uint8_t)(value >> 16U);
    bytes[offset + 3U] = (uint8_t)(value >> 24U);
}

static void repair_crc(uint8_t *bytes, size_t length)
{
    write_u32(bytes, length - 4U, scan_preview_crc32(bytes, length - 4U));
}

static uint8_t *two_voxel_vector(size_t *length_out)
{
    const size_t length = SCAN_PREVIEW_BASE_HEADER_BYTES + 4U + 2U + 4U;
    uint8_t *bytes = malloc(length);
    assert(bytes != NULL);
    memcpy(bytes, s_golden, SCAN_PREVIEW_BASE_HEADER_BYTES);
    write_u16(bytes, 26U, 3U);
    write_u16(bytes, 28U, 1U);
    write_u16(bytes, 30U, 1U);
    write_u32(bytes, 48U, 2U);
    write_u16(bytes, 54U, 0U);
    write_u16(bytes, 56U, 2U);
    bytes[58U] = 1U;
    bytes[59U] = 2U;
    repair_crc(bytes, length);
    *length_out = length;
    return bytes;
}

static void expect_error(const uint8_t *bytes, size_t length, scan_preview_error_t expected)
{
    scan_preview_t parsed;
    memset(&parsed, 0xa5, sizeof(parsed));
    const scan_preview_error_t actual = scan_preview_parse(bytes, length, NULL, &parsed);
    if (actual != expected) fprintf(stderr, "expected parser error %d, got %d\n", (int)expected, (int)actual);
    assert(actual == expected);
    const scan_preview_t empty = {0};
    assert(memcmp(&parsed, &empty, sizeof(parsed)) == 0);
}

static void test_golden_vector_and_allocator_cycles(void)
{
    counting_allocator_t counts = {0};
    const scan_preview_allocator_t allocator = {
        .alloc = counting_alloc,
        .free = counting_free,
        .context = &counts,
    };

    for (size_t i = 0U; i < 1000U; ++i) {
        scan_preview_t parsed;
        assert(scan_preview_parse(s_golden, sizeof(s_golden), &allocator, &parsed) == SCAN_PREVIEW_OK);
        assert(parsed.source_scan_crc32 == UINT32_C(0x12345678));
        assert(parsed.geometry_computed_at_ms == 0U && !parsed.registration_usable);
        assert(parsed.origin[2] == -1.0f && parsed.voxel_size_m == 0.5f);
        assert(parsed.dimensions[0] == 2U && parsed.dimensions[1] == 2U && parsed.dimensions[2] == 1U);
        assert(parsed.voxel_count == 1U && parsed.index_width == 2U && parsed.coarsening_steps == 3U);
        assert(parsed.indices[0] == 3U && parsed.face_masks[0] == 0x3fU);
        scan_preview_free(&parsed);
        assert(parsed.allocation == NULL && parsed.indices == NULL && parsed.face_masks == NULL);
        assert(counts.outstanding == 0U);
    }
    assert(counts.allocations == 1000U && counts.frees == 1000U);
}

static void test_corrupt_inputs(void)
{
    uint8_t bytes[sizeof(s_golden)];
    memcpy(bytes, s_golden, sizeof(bytes));
    bytes[0] ^= 1U;
    expect_error(bytes, sizeof(bytes), SCAN_PREVIEW_ERROR_MAGIC);

    memcpy(bytes, s_golden, sizeof(bytes));
    write_u16(bytes, 8U, 2U);
    repair_crc(bytes, sizeof(bytes));
    expect_error(bytes, sizeof(bytes), SCAN_PREVIEW_ERROR_VERSION);

    expect_error(s_golden, sizeof(s_golden) - 1U, SCAN_PREVIEW_ERROR_SIZE);

    memcpy(bytes, s_golden, sizeof(bytes));
    write_u32(bytes, 48U, UINT32_MAX);
    repair_crc(bytes, sizeof(bytes));
    expect_error(bytes, sizeof(bytes), SCAN_PREVIEW_ERROR_COUNT);

    memcpy(bytes, s_golden, sizeof(bytes));
    write_u16(bytes, 54U, 4U);
    repair_crc(bytes, sizeof(bytes));
    expect_error(bytes, sizeof(bytes), SCAN_PREVIEW_ERROR_INDEX_RANGE);

    memcpy(bytes, s_golden, sizeof(bytes));
    bytes[56U] = 0U;
    repair_crc(bytes, sizeof(bytes));
    expect_error(bytes, sizeof(bytes), SCAN_PREVIEW_ERROR_FACE_MASK);

    memcpy(bytes, s_golden, sizeof(bytes));
    bytes[sizeof(bytes) - 1U] ^= 0x80U;
    expect_error(bytes, sizeof(bytes), SCAN_PREVIEW_ERROR_CRC);

    uint8_t too_large[SCAN_PREVIEW_MAX_BYTES + 1U] = {0};
    expect_error(too_large, sizeof(too_large), SCAN_PREVIEW_ERROR_SIZE);
}

static void test_sorted_unique_indices_and_registration_validation(void)
{
    size_t length = 0U;
    uint8_t *bytes = two_voxel_vector(&length);
    scan_preview_t parsed;
    assert(scan_preview_parse(bytes, length, NULL, &parsed) == SCAN_PREVIEW_OK);
    assert(parsed.voxel_count == 2U && parsed.indices[0] == 0U && parsed.indices[1] == 2U);
    scan_preview_free(&parsed);

    bytes[56U] = 0U;
    repair_crc(bytes, length);
    expect_error(bytes, length, SCAN_PREVIEW_ERROR_INDEX_ORDER);
    bytes[56U] = 0U; bytes[57U] = 0U;
    repair_crc(bytes, length);
    expect_error(bytes, length, SCAN_PREVIEW_ERROR_INDEX_ORDER);
    free(bytes);

    bytes = malloc(sizeof(s_golden));
    assert(bytes != NULL);
    memcpy(bytes, s_golden, sizeof(s_golden));
    bytes[24U] = 2U;
    write_u16(bytes, 10U, SCAN_PREVIEW_REGISTERED_HEADER_BYTES);
    repair_crc(bytes, sizeof(s_golden));
    expect_error(bytes, sizeof(s_golden), SCAN_PREVIEW_ERROR_HEADER);
    free(bytes);
}

static void test_registration_and_allocator_failures(void)
{
    const size_t length = SCAN_PREVIEW_REGISTERED_HEADER_BYTES + 2U + 1U + 4U;
    uint8_t bytes[SCAN_PREVIEW_REGISTERED_HEADER_BYTES + 7U] = {0};
    memcpy(bytes, s_golden, SCAN_PREVIEW_BASE_HEADER_BYTES);
    write_u16(bytes, 10U, SCAN_PREVIEW_REGISTERED_HEADER_BYTES);
    write_u32(bytes, 16U, 1U);
    bytes[24U] = 1U;
    bytes[25U] = 2U;
    write_u32(bytes, 48U, 1U);
    write_u16(bytes, SCAN_PREVIEW_REGISTERED_HEADER_BYTES, 3U);
    bytes[SCAN_PREVIEW_REGISTERED_HEADER_BYTES + 2U] = 0x3fU;
    /* Identity 4x4 sensor_to_scan. */
    const uint32_t one = UINT32_C(0x3f800000);
    write_u32(bytes, 54U + 0U * 4U, one);
    write_u32(bytes, 54U + 5U * 4U, one);
    write_u32(bytes, 54U + 10U * 4U, one);
    write_u32(bytes, 54U + 15U * 4U, one);
    repair_crc(bytes, length);
    scan_preview_t parsed;
    assert(scan_preview_parse(bytes, length, NULL, &parsed) == SCAN_PREVIEW_OK);
    assert(parsed.registration_usable && parsed.geometry_computed_at_ms == 1U);
    scan_preview_free(&parsed);

    /* A registered preview must carry a nonzero geometry binding. */
    write_u32(bytes, 54U + 15U * 4U, 0U);
    repair_crc(bytes, length);
    assert(scan_preview_parse(bytes, length, NULL, &parsed) == SCAN_PREVIEW_ERROR_REGISTRATION);

    counting_allocator_t counts = {.fail = true};
    const scan_preview_allocator_t allocator = {
        .alloc = counting_alloc,
        .free = counting_free,
        .context = &counts,
    };
    assert(scan_preview_parse(s_golden, sizeof(s_golden), &allocator, &parsed) == SCAN_PREVIEW_ERROR_MEMORY);
    assert(counts.outstanding == 0U && counts.allocations == 0U);
}

int main(void)
{
    test_golden_vector_and_allocator_cycles();
    test_corrupt_inputs();
    test_sorted_unique_indices_and_registration_validation();
    test_registration_and_allocator_failures();
    puts("scan_preview parser tests passed");
    return 0;
}
