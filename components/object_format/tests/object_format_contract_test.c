#include <assert.h>
#include <stddef.h>

#include "object_format.h"

static void test_object_ids(void)
{
    static const char *valid[] = {
        "OBJ_0000",
        "OBJ_0001",
        "OBJ_9999",
    };
    static const char *invalid[] = {
        NULL,
        "",
        "OBJ_001",
        "OBJ_00000",
        "obj_0001",
        "OBJ_00A1",
        "OBJ-0001",
        "OBJ_0001/..",
    };

    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        assert(object_format_id_is_valid(valid[i]));
    }
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(!object_format_id_is_valid(invalid[i]));
    }
}

static void test_schema_versions(void)
{
    assert(!object_format_schema_is_supported(0));
    assert(!object_format_schema_is_supported(OBJECT_FORMAT_SCHEMA_VERSION - 1));
    assert(object_format_schema_is_supported(OBJECT_FORMAT_SCHEMA_VERSION));
    assert(object_format_schema_is_supported(OBJECT_FORMAT_SCHEMA_VERSION + 1));
}

static void test_metadata_identity(void)
{
    assert(object_format_metadata_identity_is_valid(
        OBJECT_FORMAT_SCHEMA_VERSION,
        "OBJ_0001",
        "OBJ_0001"
    ));
    assert(object_format_metadata_identity_is_valid(
        OBJECT_FORMAT_SCHEMA_VERSION + 1,
        "OBJ_0001",
        "OBJ_0001"
    ));
    assert(!object_format_metadata_identity_is_valid(
        0,
        "OBJ_0001",
        "OBJ_0001"
    ));
    assert(!object_format_metadata_identity_is_valid(
        OBJECT_FORMAT_SCHEMA_VERSION,
        "OBJ_0002",
        "OBJ_0001"
    ));
    assert(!object_format_metadata_identity_is_valid(
        OBJECT_FORMAT_SCHEMA_VERSION,
        "OBJ_bad",
        "OBJ_0001"
    ));
    assert(!object_format_metadata_identity_is_valid(
        OBJECT_FORMAT_SCHEMA_VERSION,
        NULL,
        "OBJ_0001"
    ));
    assert(!object_format_metadata_identity_is_valid(
        OBJECT_FORMAT_SCHEMA_VERSION,
        "OBJ_0001",
        NULL
    ));
}

int main(void)
{
    test_object_ids();
    test_schema_versions();
    test_metadata_identity();
    return 0;
}
