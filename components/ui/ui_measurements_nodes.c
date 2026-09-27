#include "ui_measurements_nodes.h"

#include <stdio.h>

static const char *ui_measurements_node_text_or_unknown(const char *text)
{
    return text != NULL ? text : "--";
}

int ui_measurements_node_format(
    char *buffer,
    size_t buffer_size,
    const char *node_name,
    bool computed,
    float x_m,
    float y_m,
    ui_measurements_node_level_t level,
    const ui_measurements_node_text_t *text)
{
    if (buffer == NULL || buffer_size == 0U || node_name == NULL || text == NULL) {
        return -1;
    }

    const char *level_text;
    switch (level) {
        case UI_MEASUREMENTS_NODE_LEVEL_LEVELED:
            level_text = text->leveled;
            break;
        case UI_MEASUREMENTS_NODE_LEVEL_TILTED:
            level_text = text->tilted;
            break;
        case UI_MEASUREMENTS_NODE_LEVEL_MOVING:
            level_text = text->moving;
            break;
        case UI_MEASUREMENTS_NODE_LEVEL_NO_DATA:
            level_text = text->no_data;
            break;
        default:
            level_text = text->no_data;
            break;
    }

    level_text = ui_measurements_node_text_or_unknown(level_text);
    if (!computed) {
        const char *unknown = ui_measurements_node_text_or_unknown(text->unknown);
        return snprintf(
            buffer,
            buffer_size,
            "%s X%s Y%s %s %s",
            node_name,
            unknown,
            unknown,
            level_text,
            ui_measurements_node_text_or_unknown(text->unsolved));
    }

    return snprintf(
        buffer,
        buffer_size,
        "%s X%.1fm Y%.1fm %s %s",
        node_name,
        (double)x_m,
        (double)y_m,
        level_text,
        ui_measurements_node_text_or_unknown(text->solved));
}
