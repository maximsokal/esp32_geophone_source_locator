#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCAN_PREVIEW_LEGACY_VERSION 1U
#define SCAN_PREVIEW_VERSION 2U
#define SCAN_PREVIEW_MAX_BYTES 65536U
#define SCAN_PREVIEW_MAX_VOXELS 65535U
#define SCAN_PREVIEW_BASE_HEADER_BYTES 54U
#define SCAN_PREVIEW_REGISTERED_HEADER_BYTES 118U

typedef enum
{
    SCAN_PREVIEW_OK = 0,
    SCAN_PREVIEW_ERROR_ARGUMENT,
    SCAN_PREVIEW_ERROR_SIZE,
    SCAN_PREVIEW_ERROR_MAGIC,
    SCAN_PREVIEW_ERROR_VERSION,
    SCAN_PREVIEW_ERROR_HEADER,
    SCAN_PREVIEW_ERROR_CRC,
    SCAN_PREVIEW_ERROR_GEOMETRY,
    SCAN_PREVIEW_ERROR_COUNT,
    SCAN_PREVIEW_ERROR_INDEX_WIDTH,
    SCAN_PREVIEW_ERROR_INDEX_RANGE,
    SCAN_PREVIEW_ERROR_INDEX_ORDER,
    SCAN_PREVIEW_ERROR_FACE_MASK,
    SCAN_PREVIEW_ERROR_REGISTRATION,
    SCAN_PREVIEW_ERROR_MEMORY,

} scan_preview_error_t;

typedef void *(*scan_preview_alloc_fn)(size_t size_bytes, void *context);
typedef void (*scan_preview_free_fn)(void *allocation, void *context);

typedef struct
{
    scan_preview_alloc_fn alloc;
    scan_preview_free_fn free;
    void *context;
} scan_preview_allocator_t;

typedef struct
{
    uint32_t source_scan_crc32;
    uint64_t geometry_computed_at_ms;
    bool registration_usable;
    float sensor_to_scan[16];
    float origin[3];
    float voxel_size_m;
    uint16_t dimensions[3];
    uint32_t voxel_count;
    uint8_t index_width;
    uint16_t coarsening_steps;
    uint32_t *indices;
    uint8_t *face_masks;

    /* One allocator-owned block holds the expanded indices and face masks. */
    void *allocation;
    scan_preview_allocator_t allocator;
} scan_preview_t;

/*
 * Parse and strictly validate preview-v1/v2.bin. Version 1 registered
 * headers are accepted with registration dropped; version 2 registrations
 * map sensor Z-up coordinates into project Z-up. A caller-supplied allocator is
 * useful for host tests; NULL selects malloc on host and PSRAM on ESP-IDF.
 * Failure leaves `out` completely zeroed.
 */
scan_preview_error_t scan_preview_parse(
    const uint8_t *data,
    size_t length,
    const scan_preview_allocator_t *allocator,
    scan_preview_t *out
);

void scan_preview_free(scan_preview_t *preview);

uint32_t scan_preview_crc32(const uint8_t *data, size_t length);
