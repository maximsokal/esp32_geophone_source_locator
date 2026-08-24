#include "object_format.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>


bool object_format_id_is_valid(
    const char *object_id
)
{
    if (
        object_id == NULL ||
        strlen(object_id) !=
            OBJECT_FORMAT_ID_TEXT_LENGTH
    ) {
        return false;
    }

    if (
        strncmp(
            object_id,
            "OBJ_",
            4
        ) != 0
    ) {
        return false;
    }

    for (
        size_t i = 4;
        i <
            OBJECT_FORMAT_ID_TEXT_LENGTH;
        ++i
    ) {
        if (
            !isdigit(
                (unsigned char)
                    object_id[i]
            )
        ) {
            return false;
        }
    }

    return true;
}


bool object_format_schema_is_supported(
    int schema_version
)
{
    return
        schema_version >=
        OBJECT_FORMAT_SCHEMA_VERSION;
}
