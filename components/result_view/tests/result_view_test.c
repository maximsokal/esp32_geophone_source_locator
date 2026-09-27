#include "result_view.h"

#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIXTURE_PATH "../../../tests/fixtures/result_view_vectors.json"
#define MAX_VECTOR_ROWS 16U
#define MAX_VECTOR_COLUMNS 29U
#define WINDOW_VECTOR_COLUMNS 21U
#define DISTANCE_VECTOR_COLUMNS 29U

static char *read_fixture(void)
{
    FILE *file = fopen(FIXTURE_PATH, "rb");
    assert(file != NULL);
    assert(fseek(file, 0L, SEEK_END) == 0);
    const long length = ftell(file);
    assert(length >= 0L);
    assert(fseek(file, 0L, SEEK_SET) == 0);

    char *text = malloc((size_t)length + 1U);
    assert(text != NULL);
    assert(fread(text, 1U, (size_t)length, file) == (size_t)length);
    text[length] = '\0';
    assert(fclose(file) == 0);
    return text;
}

static size_t read_numeric_vectors(
    const char *fixture,
    const char *section_name,
    double rows[MAX_VECTOR_ROWS][MAX_VECTOR_COLUMNS],
    size_t column_count
)
{
    const char *cursor = strstr(fixture, section_name);
    assert(cursor != NULL);
    cursor = strchr(cursor, '[');
    assert(cursor != NULL);
    ++cursor;

    size_t row_count = 0U;
    for (;;) {
        while (isspace((unsigned char)*cursor) || *cursor == ',') {
            ++cursor;
        }
        if (*cursor == ']') {
            break;
        }
        assert(*cursor == '[');
        assert(row_count < MAX_VECTOR_ROWS);
        ++cursor;

        for (size_t column = 0U; column < column_count; ++column) {
            while (isspace((unsigned char)*cursor)) {
                ++cursor;
            }
            char *end = NULL;
            rows[row_count][column] = strtod(cursor, &end);
            assert(end != cursor);
            cursor = end;
            while (isspace((unsigned char)*cursor)) {
                ++cursor;
            }
            if (column + 1U < column_count) {
                assert(*cursor == ',');
                ++cursor;
            }
        }
        assert(*cursor == ']');
        ++cursor;
        ++row_count;
    }
    return row_count;
}

static void test_window_vectors(const char *fixture)
{
    double rows[MAX_VECTOR_ROWS][MAX_VECTOR_COLUMNS] = {{0}};
    const size_t row_count = read_numeric_vectors(
        fixture, "\"windows\"", rows, WINDOW_VECTOR_COLUMNS
    );
    assert(row_count == 10U);

    for (size_t row = 0U; row < row_count; ++row) {
        const double *vector = rows[row];
        geophone_channel_pick_t picks[4] = {0};
        const size_t pick_count = (size_t)vector[1];
        assert(pick_count <= 4U);
        for (size_t i = 0U; i < pick_count; ++i) {
            const size_t column = 2U + i * 3U;
            picks[i].arrival_sample_index = (size_t)vector[column];
            picks[i].status = vector[column + 1U] == 0.0
                ? GEOPHONE_PICK_ACCEPTED
                : GEOPHONE_PICK_REJECTED_LOW_SNR;
            picks[i].snr = (float)vector[column + 2U];
        }

        result_view_window_t window = {0};
        const result_view_status_t status = result_view_segment_window(
            picks,
            pick_count,
            (uint32_t)vector[14],
            (size_t)vector[15],
            vector[16],
            vector[17],
            &window
        );
        if (vector[18] == 0.0) {
            assert(status == RESULT_VIEW_UNAVAILABLE);
            assert(!window.available);
            assert(window.start_frame == 0U);
            assert(window.frame_count == 0U);
        } else {
            assert(status == RESULT_VIEW_OK);
            assert(window.available);
            assert(window.start_frame == (size_t)vector[19]);
            assert(window.frame_count == (size_t)vector[20]);
        }
    }
}

static void test_window_invalid_inputs(void)
{
    geophone_channel_pick_t pick = {.status = GEOPHONE_PICK_ACCEPTED, .arrival_sample_index = 4U};
    result_view_window_t window = {.available = true, .start_frame = 9U, .frame_count = 9U};
    assert(result_view_segment_window(&pick, 1U, 0U, 10U, 0.5, 1.0, &window) == RESULT_VIEW_INVALID_ARGUMENT);
    assert(!window.available && window.start_frame == 0U && window.frame_count == 0U);
    assert(result_view_segment_window(&pick, 1U, 10U, 10U, -0.5, 1.0, &window) == RESULT_VIEW_INVALID_ARGUMENT);
    pick.arrival_sample_index = 10U;
    assert(result_view_segment_window(&pick, 1U, 10U, 10U, 0.5, 1.0, &window) == RESULT_VIEW_INVALID_ARGUMENT);
    assert(result_view_segment_window(NULL, 0U, 10U, 100U, 0.5, 1.0, &window) == RESULT_VIEW_UNAVAILABLE);
}

static void test_marker_offsets(void)
{
    result_view_window_t window = {.available = true, .start_frame = 100U, .frame_count = 20U};
    geophone_channel_pick_t pick = {.status = GEOPHONE_PICK_ACCEPTED, .arrival_sample_index = 110U};
    size_t offset = 99U;
    assert(result_view_arrival_marker_offset(&pick, &window, &offset));
    assert(offset == 10U);

    pick.status = GEOPHONE_PICK_REJECTED_LOW_SNR;
    assert(!result_view_arrival_marker_offset(&pick, &window, &offset));
    assert(offset == 0U);
    pick.status = GEOPHONE_PICK_ACCEPTED;
    pick.arrival_sample_index = 120U;
    assert(!result_view_arrival_marker_offset(&pick, &window, &offset));
    window.available = false;
    assert(!result_view_arrival_marker_offset(&pick, &window, &offset));
}

static void test_distance_vectors(const char *fixture)
{
    double rows[MAX_VECTOR_ROWS][MAX_VECTOR_COLUMNS] = {{0}};
    const size_t row_count = read_numeric_vectors(
        fixture, "\"distances\"", rows, DISTANCE_VECTOR_COLUMNS
    );
    assert(row_count == 2U);

    for (size_t row = 0U; row < row_count; ++row) {
        const double *vector = rows[row];
        const size_t pod_count = (size_t)vector[1];
        object_store_pod_geometry_t pods[OBJECT_STORE_POD_COUNT] = {0};
        result_view_distance_t distances[OBJECT_STORE_POD_COUNT] = {0};
        for (size_t i = 0U; i < pod_count; ++i) {
            const size_t column = 5U + i * 4U;
            pods[i].computed = vector[column] != 0.0;
            pods[i].x_mm = (int32_t)vector[column + 1U];
            pods[i].y_mm = (int32_t)vector[column + 2U];
            pods[i].z_mm = (int32_t)vector[column + 3U];
        }
        const double source_m[3] = {vector[2], vector[3], vector[4]};
        assert(result_view_source_distances(source_m, pods, pod_count, distances) == RESULT_VIEW_OK);
        for (size_t i = 0U; i < pod_count; ++i) {
            const size_t expected_column = 21U + i * 2U;
            if (vector[expected_column] == 0.0) {
                assert(!distances[i].known);
                assert(isnan(distances[i].distance_m));
            } else {
                assert(distances[i].known);
                assert(fabs(distances[i].distance_m - vector[expected_column + 1U]) < 1.0e-6);
            }
        }
    }
}

static void test_distance_invalid_inputs(void)
{
    const double source_m[3] = {0.0, 0.0, 0.0};
    object_store_pod_geometry_t pods[OBJECT_STORE_POD_COUNT] = {0};
    result_view_distance_t distances[OBJECT_STORE_POD_COUNT] = {0};
    assert(result_view_source_distances(source_m, pods, OBJECT_STORE_POD_COUNT + 1U, distances) == RESULT_VIEW_INVALID_ARGUMENT);
    const double invalid_source_m[3] = {NAN, 0.0, 0.0};
    assert(result_view_source_distances(invalid_source_m, pods, OBJECT_STORE_POD_COUNT, distances) == RESULT_VIEW_INVALID_ARGUMENT);
    assert(!distances[0].known && isnan(distances[0].distance_m));
}

int main(void)
{
    char *fixture = read_fixture();
    test_window_vectors(fixture);
    test_window_invalid_inputs();
    test_marker_offsets();
    test_distance_vectors(fixture);
    test_distance_invalid_inputs();
    free(fixture);
    return 0;
}
