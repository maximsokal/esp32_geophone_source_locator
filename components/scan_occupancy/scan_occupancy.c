#include "scan_occupancy.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"


#define FIXED_HEADER_BYTES 20U
static const uint8_t MAGIC[8] = { 'G', 'O', 'C', 'C', 'U', 'P', 'Y', '1' };


static uint32_t read_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
        ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) |
        ((uint32_t)bytes[3] << 24);
}


static int16_t read_i16(const uint8_t *bytes)
{
    return (int16_t)(uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}


static uint32_t crc32(const uint8_t *bytes, size_t length)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0U; i < length; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return crc ^ 0xFFFFFFFFU;
}


static int64_t pack_key(int32_t ix, int32_t iy, int32_t iz)
{
    /* Each index fits int16; offset to unsigned 16-bit fields. */
    return ((int64_t)(ix + 32768) << 32) | ((int64_t)(iy + 32768) << 16) | (int64_t)(iz + 32768);
}


static int compare_keys(const void *a, const void *b)
{
    const int64_t left = *(const int64_t *)a;
    const int64_t right = *(const int64_t *)b;
    return left < right ? -1 : (left > right ? 1 : 0);
}


static bool contains_key(const int64_t *keys, size_t count, int64_t key)
{
    size_t low = 0U;
    size_t high = count;
    while (low < high) {
        const size_t mid = low + (high - low) / 2U;
        if (keys[mid] == key) {
            return true;
        }
        if (keys[mid] < key) {
            low = mid + 1U;
        } else {
            high = mid;
        }
    }
    return false;
}


static bool read_number(const cJSON *object, const char *key, double *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)) {
        return false;
    }
    *out = item->valuedouble;
    return true;
}


static bool read_vec3(const cJSON *array, double out[3])
{
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) != 3) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        const cJSON *item = cJSON_GetArrayItem(array, i);
        if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)) {
            return false;
        }
        out[i] = item->valuedouble;
    }
    return true;
}


static bool parse_registration(
    const cJSON *item,
    uint32_t schema,
    scan_occupancy_registration_t *out
)
{
    memset(out, 0, sizeof(*out));
    if (item == NULL || cJSON_IsNull(item)) {
        /* null/absent: a valid artifact whose constraint stays disabled. */
        return true;
    }
    if (!cJSON_IsObject(item)) return false;
    const cJSON *source = cJSON_GetObjectItemCaseSensitive(item, "source");
    const cJSON *matrix = cJSON_GetObjectItemCaseSensitive(item, "sensor_to_scan");
    const cJSON *frame = cJSON_GetObjectItemCaseSensitive(item, "frame");
    double geometry_ms = 0.0;
    double residual = 0.0;
    if (
        !cJSON_IsString(source) ||
        !read_number(item, "geometry_computed_at_ms", &geometry_ms) || geometry_ms < 0.0 ||
        !read_number(item, "residual_m", &residual) ||
        !cJSON_IsArray(matrix) || cJSON_GetArraySize(matrix) != 16 ||
        (schema == SCAN_OCCUPANCY_SCHEMA &&
         (!cJSON_IsString(frame) || strcmp(frame->valuestring, "project_z_up") != 0))
    ) {
        return false;
    }
    for (int i = 0; i < 16; ++i) {
        const cJSON *value = cJSON_GetArrayItem(matrix, i);
        if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble)) {
            return false;
        }
        out->sensor_to_scan[i] = value->valuedouble;
    }
    snprintf(out->source, sizeof(out->source), "%s", source->valuestring);
    out->geometry_computed_at_ms = (uint64_t)geometry_ms;
    out->residual_m = residual;
    if (schema == SCAN_OCCUPANCY_SCHEMA) {
        out->present = true;
    } else {
        memset(out, 0, sizeof(*out));
    }
    return true;
}


static esp_err_t parse_header(const char *json, size_t length, scan_occupancy_t *out, size_t *free_count, size_t *occupied_count)
{
    /* NUL-terminated copy: the header is not terminated inside the file. */
    char *text = malloc(length + 1U);
    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(text, json, length);
    text[length] = '\0';
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    double schema_value = 0.0;
    double voxel = 0.0;
    double below = 0.0;
    double frees = 0.0;
    double occupieds = 0.0;
    double created = 0.0;
    const cJSON *frame_id = cJSON_GetObjectItemCaseSensitive(root, "frame_id");
    const cJSON *envelope = cJSON_GetObjectItemCaseSensitive(root, "envelope");
    const cJSON *saturated = cJSON_GetObjectItemCaseSensitive(root, "saturated");

    esp_err_t err = ESP_OK;
    if (!read_number(root, "schema", &schema_value) ||
        (schema_value != (double)SCAN_OCCUPANCY_SCHEMA &&
         schema_value != (double)SCAN_OCCUPANCY_LEGACY_SCHEMA)) {
        err = ESP_ERR_INVALID_VERSION;
    } else if (
        !cJSON_IsString(frame_id) || frame_id->valuestring[0] == '\0' ||
        !read_number(root, "voxel_size_m", &voxel) || !(voxel > 0.0) ||
        !read_number(root, "below_surface_m", &below) || below < 0.0 ||
        !read_number(root, "free_count", &frees) || frees < 0.0 ||
        !read_number(root, "occupied_count", &occupieds) || occupieds < 0.0 ||
        !read_number(root, "created_at_ms", &created) || created < 0.0 ||
        !parse_registration(
            cJSON_GetObjectItemCaseSensitive(root, "registration"),
            (uint32_t)schema_value,
            &out->registration)
    ) {
        err = ESP_ERR_INVALID_ARG;
    } else {
        snprintf(out->frame_id, sizeof(out->frame_id), "%s", frame_id->valuestring);
        out->voxel_size_m = voxel;
        out->below_surface_m = below;
        out->saturated = cJSON_IsTrue(saturated);
        out->created_at_ms = (uint64_t)created;
        out->has_envelope = false;
        if (cJSON_IsObject(envelope)) {
            if (
                !read_vec3(cJSON_GetObjectItemCaseSensitive(envelope, "min"), out->envelope_min) ||
                !read_vec3(cJSON_GetObjectItemCaseSensitive(envelope, "max"), out->envelope_max)
            ) {
                err = ESP_ERR_INVALID_ARG;
            } else {
                out->has_envelope = true;
            }
        }
        *free_count = (size_t)frees;
        *occupied_count = (size_t)occupieds;
    }

    cJSON_Delete(root);
    return err;
}


static int64_t *read_keys(const uint8_t *payload, size_t count)
{
    int64_t *keys = malloc((count > 0U ? count : 1U) * sizeof(int64_t));
    if (keys == NULL) {
        return NULL;
    }
    for (size_t i = 0U; i < count; ++i) {
        const uint8_t *cell = payload + i * 6U;
        keys[i] = pack_key(read_i16(cell), read_i16(cell + 2), read_i16(cell + 4));
    }
    qsort(keys, count, sizeof(int64_t), compare_keys);
    return keys;
}


esp_err_t scan_occupancy_parse(
    const uint8_t *data,
    size_t length,
    scan_occupancy_t *out
)
{
    if (data == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    if (length < FIXED_HEADER_BYTES || length > SCAN_OCCUPANCY_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (memcmp(data, MAGIC, sizeof(MAGIC)) != 0) {
        return ESP_ERR_INVALID_VERSION;
    }
    const uint32_t header_length = read_u32(data + 8);
    const uint32_t payload_length = read_u32(data + 12);
    if (
        header_length == 0U ||
        (uint64_t)FIXED_HEADER_BYTES + header_length + payload_length != (uint64_t)length
    ) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (crc32(data + FIXED_HEADER_BYTES, length - FIXED_HEADER_BYTES) != read_u32(data + 16)) {
        return ESP_ERR_INVALID_CRC;
    }

    size_t free_count = 0U;
    size_t occupied_count = 0U;
    esp_err_t err = parse_header(
        (const char *)(data + FIXED_HEADER_BYTES), header_length, out, &free_count, &occupied_count
    );
    if (err != ESP_OK) {
        memset(out, 0, sizeof(*out));
        return err;
    }
    if ((uint64_t)payload_length != 6ULL * ((uint64_t)free_count + (uint64_t)occupied_count)) {
        memset(out, 0, sizeof(*out));
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t *payload = data + FIXED_HEADER_BYTES + header_length;
    out->free_keys = read_keys(payload, free_count);
    out->occupied_keys = read_keys(payload + 6U * free_count, occupied_count);
    if (out->free_keys == NULL || out->occupied_keys == NULL) {
        scan_occupancy_free(out);
        return ESP_ERR_NO_MEM;
    }
    out->free_count = free_count;
    out->occupied_count = occupied_count;
    return ESP_OK;
}


void scan_occupancy_free(scan_occupancy_t *occupancy)
{
    if (occupancy == NULL) {
        return;
    }
    free(occupancy->free_keys);
    free(occupancy->occupied_keys);
    memset(occupancy, 0, sizeof(*occupancy));
}


scan_occupancy_class_t scan_occupancy_classify(
    const scan_occupancy_t *occupancy,
    const double point_scan[3]
)
{
    if (occupancy == NULL || point_scan == NULL || !(occupancy->voxel_size_m > 0.0)) {
        return SCAN_OCCUPANCY_UNKNOWN;
    }

    int32_t index[3];
    for (int axis = 0; axis < 3; ++axis) {
        if (!isfinite(point_scan[axis])) {
            return SCAN_OCCUPANCY_UNKNOWN;
        }
        const double cell = floor(point_scan[axis] / occupancy->voxel_size_m);
        if (cell < -32768.0 || cell > 32767.0) {
            return occupancy->has_envelope ? SCAN_OCCUPANCY_OUTSIDE : SCAN_OCCUPANCY_UNKNOWN;
        }
        index[axis] = (int32_t)cell;
    }

    const int64_t key = pack_key(index[0], index[1], index[2]);
    if (contains_key(occupancy->free_keys, occupancy->free_count, key)) {
        return SCAN_OCCUPANCY_FREE;
    }
    if (contains_key(occupancy->occupied_keys, occupancy->occupied_count, key)) {
        return SCAN_OCCUPANCY_OCCUPIED;
    }

    if (occupancy->has_envelope) {
        for (int axis = 0; axis < 3; ++axis) {
            const double low = occupancy->envelope_min[axis] - (axis == 2 ? occupancy->below_surface_m : 0.0);
            if (point_scan[axis] < low || point_scan[axis] > occupancy->envelope_max[axis]) {
                return SCAN_OCCUPANCY_OUTSIDE;
            }
        }
    }
    return SCAN_OCCUPANCY_UNKNOWN;
}


bool scan_occupancy_registration_usable(
    const scan_occupancy_t *occupancy,
    uint64_t geometry_computed_at_ms
)
{
    if (
        occupancy == NULL ||
        !occupancy->registration.present ||
        geometry_computed_at_ms == 0U ||
        occupancy->registration.geometry_computed_at_ms != geometry_computed_at_ms
    ) {
        return false;
    }

    const double *m = occupancy->registration.sensor_to_scan;
    if (m[12] != 0.0 || m[13] != 0.0 || m[14] != 0.0 || m[15] != 1.0) {
        return false;
    }
    /* Rotation block must be orthonormal (|det| = 1; a mirror is allowed). */
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            double dot = 0.0;
            for (int k = 0; k < 3; ++k) {
                dot += m[r * 4 + k] * m[c * 4 + k];
            }
            if (fabs(dot - (r == c ? 1.0 : 0.0)) > 1e-3) {
                return false;
            }
        }
    }
    return true;
}


void scan_occupancy_sensor_to_scan(
    const scan_occupancy_t *occupancy,
    const double point_sensor[3],
    double point_scan[3]
)
{
    const double *m = occupancy->registration.sensor_to_scan;
    for (int r = 0; r < 3; ++r) {
        point_scan[r] =
            m[r * 4 + 0] * point_sensor[0] +
            m[r * 4 + 1] * point_sensor[1] +
            m[r * 4 + 2] * point_sensor[2] +
            m[r * 4 + 3];
    }
}
