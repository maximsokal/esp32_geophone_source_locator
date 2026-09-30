#include "nrf_log_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static FILE *open_corpus_file(const char *filename)
{
    const char *source_file = __FILE__;
    const char *separator = strrchr(source_file, '/');
    const char *backslash = strrchr(source_file, '\\');
    char path[sizeof(__FILE__) + sizeof("/data/nrf_log_corpus/") + 64U];
    size_t directory_length;
    int path_length;

    if (backslash != NULL && (separator == NULL || backslash > separator)) separator = backslash;
    CHECK(separator != NULL);
    directory_length = (size_t)(separator - source_file);
    path_length = snprintf(
        path,
        sizeof(path),
        "%.*s/data/nrf_log_corpus/%s",
        (int)directory_length,
        source_file,
        filename);
    CHECK(path_length > 0 && (size_t)path_length < sizeof(path));
    return fopen(path, "r");
}

static size_t load_hex_frame(const char *filename, uint8_t *frame, size_t capacity)
{
    FILE *file = open_corpus_file(filename);
    size_t length = 0U;
    int scanned;
    unsigned int value;

    CHECK(file != NULL);
    while ((scanned = fscanf(file, "%2x", &value)) == 1) {
        CHECK(length < capacity);
        CHECK(value <= UINT8_MAX);
        frame[length++] = (uint8_t)value;
    }
    CHECK(scanned == EOF);
    CHECK(ferror(file) == 0);
    CHECK(fclose(file) == 0);
    CHECK(length != 0U);
    return length;
}

static nrf_log_error_t expected_error_from_filename(const char *filename)
{
    if (strncmp(filename, "OK-", 3U) == 0) return NRF_LOG_ERROR_OK;
    if (strncmp(filename, "BAD_LENGTH-", 11U) == 0) return NRF_LOG_ERROR_BAD_LENGTH;
    if (strncmp(filename, "BAD_MAGIC-", 10U) == 0) return NRF_LOG_ERROR_BAD_MAGIC;
    if (strncmp(filename, "RESERVED_NONZERO-", 17U) == 0) return NRF_LOG_ERROR_RESERVED_NONZERO;
    fprintf(stderr, "unknown expected result in corpus filename: %s\n", filename);
    exit(EXIT_FAILURE);
}

int main(void)
{
    static const char *const files[] = {
        "OK-block-max-136.hex",
        "BAD_LENGTH-block-137.hex",
        "BAD_LENGTH-block-truncated-116.hex",
        "BAD_MAGIC-block-all-ff-136.hex",
        "RESERVED_NONZERO-block-reserved-valid-crc.hex",
    };

    for (size_t file_index = 0U; file_index < sizeof(files) / sizeof(files[0]); ++file_index) {
        uint8_t frame[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH + 1U];
        nrf_log_block_t block;
        const size_t length = load_hex_frame(files[file_index], frame, sizeof(frame));
        const nrf_log_error_t actual = nrf_log_protocol_decode_block(frame, length, NULL, &block);
        const nrf_log_error_t expected = expected_error_from_filename(files[file_index]);

        if (actual != expected) {
            fprintf(stderr, "%s: expected error %d, got %d\n", files[file_index], expected, actual);
            return EXIT_FAILURE;
        }
        if (actual == NRF_LOG_ERROR_OK) {
            CHECK(length == NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH);
            CHECK(block.record_count == NRF_LOG_PROTOCOL_MAX_RECORDS);
            CHECK(block.next_cursor == block.first_seq + (uint32_t)block.record_count);
            for (uint8_t i = 0U; i < block.record_count; ++i) {
                CHECK(block.records[i].seq == block.first_seq + i);
                if (i != 0U) CHECK(block.records[i].seq > block.records[i - 1U].seq);
            }
        }
    }
    puts("nrf_log_corpus_replay_test: PASS");
    return EXIT_SUCCESS;
}
