#pragma once

#include <stdbool.h>
#include <stddef.h>

#define UI_MEASUREMENTS_NODE_COUNT 5U

typedef enum {
    UI_MEASUREMENTS_NODE_LEVEL_NO_DATA = 0,
    UI_MEASUREMENTS_NODE_LEVEL_LEVELED,
    UI_MEASUREMENTS_NODE_LEVEL_TILTED,
    UI_MEASUREMENTS_NODE_LEVEL_MOVING,
} ui_measurements_node_level_t;

typedef struct {
    const char *unknown;
    const char *no_data;
    const char *leveled;
    const char *tilted;
    const char *moving;
    const char *solved;
    const char *unsolved;
} ui_measurements_node_text_t;

/* Pure row formatter. Coordinates are shown in metres to one decimal place. */
int ui_measurements_node_format(
    char *buffer,
    size_t buffer_size,
    const char *node_name,
    bool computed,
    float x_m,
    float y_m,
    ui_measurements_node_level_t level,
    const ui_measurements_node_text_t *text);
