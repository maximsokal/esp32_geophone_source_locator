/*
 * SD card path safety. Object ids arrive over HTTP and are used to build file
 * system paths on the card, so the id validator is the gate: exactly "OBJ_" and
 * four ASCII digits, nothing else. Every byte value at every position, every
 * length, and the identity check that ties a stored file to its folder.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "object_format.h"

#define CASE_PASS(name) printf("PERIPHERAL_CASE %s PASS\n", name)

static void every_byte_at_every_position_is_classified(void)
{
    static const char valid[] = "OBJ_0427";
    unsigned accepted = 0U;

    assert(object_format_id_is_valid(valid));
    for (size_t position = 0U; position < OBJECT_FORMAT_ID_TEXT_LENGTH; ++position) {
        for (unsigned value = 1U; value <= 255U; ++value) {
            char candidate[OBJECT_FORMAT_ID_TEXT_LENGTH + 1U];
            memcpy(candidate, valid, sizeof(candidate));
            candidate[position] = (char)value;
            const bool expected = position < 4U
                ? (char)value == valid[position]
                : (value >= '0' && value <= '9');
            assert(object_format_id_is_valid(candidate) == expected);
            if (expected) ++accepted;
        }
        /* A shortened string is never valid, whatever the cut. */
        char cut[OBJECT_FORMAT_ID_TEXT_LENGTH + 1U];
        memcpy(cut, valid, sizeof(cut));
        cut[position] = '\0';
        assert(!object_format_id_is_valid(cut));
    }
    /* 4 fixed positions accept 1 value each, 4 digit positions accept 10 each. */
    assert(accepted == 4U * 1U + 4U * 10U);
    CASE_PASS("SD.object_id_byte_domain");
}

static void length_and_traversal_attempts_are_rejected(void)
{
    char buffer[64];
    for (size_t length = 0U; length < sizeof(buffer) - 1U; ++length) {
        memset(buffer, '1', sizeof(buffer));
        memcpy(buffer, "OBJ_", length < 4U ? length : 4U);
        buffer[length] = '\0';
        assert(object_format_id_is_valid(buffer) == (length == OBJECT_FORMAT_ID_TEXT_LENGTH && length >= 8U));
    }
    static const char *const hostile[] = {
        "OBJ_0001/..", "OBJ_0001/", "/OBJ_0001", "../OBJ_0001", "OBJ_..01", "OBJ_0001\\..",
        "OBJ_0001%2e", "OBJ_00 1", "OBJ_+001", "OBJ_-001", "OBJ_0x01", "OBJ_1e03", "OBJ_\xD9\xA0\xD9\xA0\xD9\xA0\xD9\xA1",
        "\xEF\xBC\x8F" "OBJ_0001", "OBJ_0001\n", "OBJ_0001\r", " OBJ_0001", "OBJ_0001 ",
        "OBJ_", "OBJ_00000001", "O_B_0001", "OBJ_0001:", "C:\\OBJ_001",
    };
    for (size_t i = 0U; i < sizeof(hostile) / sizeof(hostile[0]); ++i) {
        assert(!object_format_id_is_valid(hostile[i]));
    }
    assert(!object_format_id_is_valid(NULL));
    CASE_PASS("SD.object_id_traversal");
}

static void stored_files_must_match_their_folder_and_schema(void)
{
    const char *const a = "OBJ_0001";
    const char *const b = "OBJ_0002";

    assert(object_format_metadata_identity_is_valid(OBJECT_FORMAT_SCHEMA_VERSION, a, a));
    assert(object_format_metadata_identity_is_valid(OBJECT_FORMAT_SCHEMA_VERSION + 1, a, a));
    assert(!object_format_metadata_identity_is_valid(OBJECT_FORMAT_SCHEMA_VERSION, a, b));
    assert(!object_format_metadata_identity_is_valid(OBJECT_FORMAT_SCHEMA_VERSION - 1, a, a));
    assert(!object_format_metadata_identity_is_valid(0, a, a));
    assert(!object_format_metadata_identity_is_valid(-1, a, a));
    assert(!object_format_metadata_identity_is_valid(OBJECT_FORMAT_SCHEMA_VERSION, NULL, a));
    assert(!object_format_metadata_identity_is_valid(OBJECT_FORMAT_SCHEMA_VERSION, a, NULL));
    assert(!object_format_metadata_identity_is_valid(OBJECT_FORMAT_SCHEMA_VERSION, "OBJ_000A", "OBJ_000A"));
    for (int version = -3; version <= OBJECT_FORMAT_SCHEMA_VERSION + 3; ++version) {
        assert(object_format_schema_is_supported(version) == (version >= OBJECT_FORMAT_SCHEMA_VERSION));
    }
    CASE_PASS("SD.metadata_identity");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    every_byte_at_every_position_is_classified();
    length_and_traversal_attempts_are_rejected();
    stored_files_must_match_their_folder_and_schema();
    return 0;
}
