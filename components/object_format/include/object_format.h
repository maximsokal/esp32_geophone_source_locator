#pragma once

#include <stdbool.h>


#define OBJECT_FORMAT_ID_SIZE          16
#define OBJECT_FORMAT_NAME_SIZE        64
#define OBJECT_FORMAT_MAX_OBJECTS      32
#define OBJECT_FORMAT_ID_TEXT_LENGTH    8
#define OBJECT_FORMAT_SCHEMA_VERSION    1


typedef struct
{
    char id[
        OBJECT_FORMAT_ID_SIZE
    ];

    char name[
        OBJECT_FORMAT_NAME_SIZE
    ];

    bool active;

} object_summary_t;


bool object_format_id_is_valid(
    const char *object_id
);


bool object_format_schema_is_supported(
    int schema_version
);


/*
 * Canonical committed-object identity contract shared by runtime object-store
 * admission and storage recovery. JSON parsing stays in the owning component;
 * this helper only defines the semantic schema/id policy.
 */
bool object_format_metadata_identity_is_valid(
    int schema_version,
    const char *embedded_id,
    const char *expected_id
);
