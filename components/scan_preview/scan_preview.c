#include "scan_preview.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

static const uint8_t s_magic[8] = {'G', 'X', 'P', 'R', 'V', '1', 0, 0};

static uint16_t read_u16(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8U));
}

static uint32_t read_u32(const uint8_t *data)
{
    return (uint32_t)data[0] |
        ((uint32_t)data[1] << 8U) |
        ((uint32_t)data[2] << 16U) |
        ((uint32_t)data[3] << 24U);
}

static uint64_t read_u64(const uint8_t *data)
{
    return (uint64_t)read_u32(data) | ((uint64_t)read_u32(data + 4U) << 32U);
}

static float read_f32(const uint8_t *data)
{
    const uint32_t bits = read_u32(data);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

uint32_t scan_preview_crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = UINT32_C(0xffffffff);
    if (data == NULL && length != 0U) return 0U;
    for (size_t i = 0U; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return crc ^ UINT32_C(0xffffffff);
}

static void *default_alloc(size_t size_bytes, void *context)
{
    (void)context;
#ifdef ESP_PLATFORM
    return heap_caps_malloc(size_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(size_bytes);
#endif
}

static void default_free(void *allocation, void *context)
{
    (void)context;
#ifdef ESP_PLATFORM
    heap_caps_free(allocation);
#else
    free(allocation);
#endif
}

static bool matrix_is_rigid(const float matrix[16])
{
    if (matrix[12] != 0.0f || matrix[13] != 0.0f || matrix[14] != 0.0f || matrix[15] != 1.0f) return false;
    for (size_t i = 0U; i < 16U; ++i) if (!isfinite(matrix[i])) return false;

    for (size_t row = 0U; row < 3U; ++row) {
        for (size_t other = 0U; other < 3U; ++other) {
            const float dot =
                matrix[row * 4U] * matrix[other * 4U] +
                matrix[row * 4U + 1U] * matrix[other * 4U + 1U] +
                matrix[row * 4U + 2U] * matrix[other * 4U + 2U];
            const float expected = row == other ? 1.0f : 0.0f;
            if (fabsf(dot - expected) > 0.002f) return false;
        }
    }

    const float determinant =
        matrix[0] * (matrix[5] * matrix[10] - matrix[6] * matrix[9]) -
        matrix[1] * (matrix[4] * matrix[10] - matrix[6] * matrix[8]) +
        matrix[2] * (matrix[4] * matrix[9] - matrix[5] * matrix[8]);
    return fabsf(fabsf(determinant) - 1.0f) <= 0.002f;
}

void scan_preview_free(scan_preview_t *preview)
{
    if (preview == NULL) return;
    if (preview->allocation != NULL && preview->allocator.free != NULL) {
        preview->allocator.free(preview->allocation, preview->allocator.context);
    }
    memset(preview, 0, sizeof(*preview));
}

static scan_preview_error_t parse_internal(
    const uint8_t *data,
    size_t length,
    const scan_preview_allocator_t *allocator,
    scan_preview_t *parsed
)
{
    if (data == NULL || parsed == NULL) return SCAN_PREVIEW_ERROR_ARGUMENT;
    if (length < SCAN_PREVIEW_BASE_HEADER_BYTES + 4U || length > SCAN_PREVIEW_MAX_BYTES) return SCAN_PREVIEW_ERROR_SIZE;
    if (memcmp(data, s_magic, sizeof(s_magic)) != 0) return SCAN_PREVIEW_ERROR_MAGIC;
    if (read_u16(data + 8U) != SCAN_PREVIEW_VERSION) return SCAN_PREVIEW_ERROR_VERSION;

    const uint8_t registration_flag = data[24U];
    if (registration_flag > 1U) return SCAN_PREVIEW_ERROR_HEADER;
    const uint16_t header_length = read_u16(data + 10U);
    const uint16_t expected_header_length = registration_flag != 0U
        ? SCAN_PREVIEW_REGISTERED_HEADER_BYTES
        : SCAN_PREVIEW_BASE_HEADER_BYTES;
    if (header_length != expected_header_length) return SCAN_PREVIEW_ERROR_HEADER;

    parsed->source_scan_crc32 = read_u32(data + 12U);
    parsed->geometry_computed_at_ms = read_u64(data + 16U);
    parsed->registration_usable = registration_flag != 0U;
    parsed->index_width = data[25U];
    parsed->dimensions[0] = read_u16(data + 26U);
    parsed->dimensions[1] = read_u16(data + 28U);
    parsed->dimensions[2] = read_u16(data + 30U);
    for (size_t axis = 0U; axis < 3U; ++axis) parsed->origin[axis] = read_f32(data + 32U + axis * 4U);
    parsed->voxel_size_m = read_f32(data + 44U);
    parsed->voxel_count = read_u32(data + 48U);
    parsed->coarsening_steps = read_u16(data + 52U);

    if (!isfinite(parsed->origin[0]) || !isfinite(parsed->origin[1]) || !isfinite(parsed->origin[2]) ||
        !isfinite(parsed->voxel_size_m) || parsed->voxel_size_m <= 0.0f ||
        parsed->dimensions[0] == 0U || parsed->dimensions[1] == 0U || parsed->dimensions[2] == 0U) {
        return SCAN_PREVIEW_ERROR_GEOMETRY;
    }
    if (parsed->registration_usable) {
        if (parsed->geometry_computed_at_ms == 0U) return SCAN_PREVIEW_ERROR_REGISTRATION;
        for (size_t i = 0U; i < 16U; ++i) parsed->sensor_to_scan[i] = read_f32(data + SCAN_PREVIEW_BASE_HEADER_BYTES + i * 4U);
        if (!matrix_is_rigid(parsed->sensor_to_scan)) return SCAN_PREVIEW_ERROR_REGISTRATION;
    } else if (parsed->geometry_computed_at_ms != 0U) {
        return SCAN_PREVIEW_ERROR_REGISTRATION;
    }

    const uint64_t volume = (uint64_t)parsed->dimensions[0] * parsed->dimensions[1] * parsed->dimensions[2];
    if (volume == 0U || volume > (UINT64_C(1) << 32U)) return SCAN_PREVIEW_ERROR_GEOMETRY;
    const uint8_t expected_index_width = volume <= UINT64_C(65536) ? 2U : 4U;
    if (parsed->index_width != expected_index_width) return SCAN_PREVIEW_ERROR_INDEX_WIDTH;
    if (parsed->voxel_count > SCAN_PREVIEW_MAX_VOXELS || parsed->voxel_count > volume) return SCAN_PREVIEW_ERROR_COUNT;
    const uint64_t expected_length = (uint64_t)header_length +
        (uint64_t)parsed->voxel_count * (uint64_t)(parsed->index_width + 1U) + 4U;
    if (expected_length != length) return expected_length > length
        ? SCAN_PREVIEW_ERROR_SIZE
        : SCAN_PREVIEW_ERROR_COUNT;
    const uint32_t expected_crc = read_u32(data + length - 4U);
    if (scan_preview_crc32(data, length - 4U) != expected_crc) return SCAN_PREVIEW_ERROR_CRC;

    scan_preview_allocator_t selected_allocator = allocator != NULL
        ? *allocator
        : (scan_preview_allocator_t){ .alloc = default_alloc, .free = default_free, .context = NULL };
    if (selected_allocator.alloc == NULL || selected_allocator.free == NULL) return SCAN_PREVIEW_ERROR_ARGUMENT;
    parsed->allocator = selected_allocator;
    if (parsed->voxel_count > 0U) {
        const size_t index_bytes = (size_t)parsed->voxel_count * sizeof(uint32_t);
        const size_t allocation_bytes = index_bytes + (size_t)parsed->voxel_count;
        parsed->allocation = selected_allocator.alloc(allocation_bytes, selected_allocator.context);
        if (parsed->allocation == NULL) return SCAN_PREVIEW_ERROR_MEMORY;
        parsed->indices = parsed->allocation;
        parsed->face_masks = (uint8_t *)parsed->allocation + index_bytes;
    }

    size_t offset = header_length;
    uint64_t previous = 0U;
    for (uint32_t i = 0U; i < parsed->voxel_count; ++i) {
        const uint32_t index = parsed->index_width == 2U
            ? read_u16(data + offset)
            : read_u32(data + offset);
        offset += parsed->index_width;
        if ((uint64_t)index >= volume) return SCAN_PREVIEW_ERROR_INDEX_RANGE;
        if (i > 0U && (uint64_t)index <= previous) return SCAN_PREVIEW_ERROR_INDEX_ORDER;
        parsed->indices[i] = index;
        previous = index;
    }
    for (uint32_t i = 0U; i < parsed->voxel_count; ++i) {
        const uint8_t mask = data[offset + i];
        if (mask == 0U || (mask & 0xc0U) != 0U) return SCAN_PREVIEW_ERROR_FACE_MASK;
        parsed->face_masks[i] = mask;
    }
    return SCAN_PREVIEW_OK;
}

scan_preview_error_t scan_preview_parse(
    const uint8_t *data,
    size_t length,
    const scan_preview_allocator_t *allocator,
    scan_preview_t *out
)
{
    if (out == NULL) return SCAN_PREVIEW_ERROR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    scan_preview_t parsed = {0};
    const scan_preview_error_t error = parse_internal(data, length, allocator, &parsed);
    if (error != SCAN_PREVIEW_OK) {
        scan_preview_free(&parsed);
        return error;
    }
    *out = parsed;
    return SCAN_PREVIEW_OK;
}
