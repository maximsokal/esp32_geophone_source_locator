#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "nrf_node_link_status_policy.h"
#include "ui_i18n_catalog.gen.h"

typedef enum {
    UI_NODE_LINK_NODE_MAIN = 0U,
    UI_NODE_LINK_NODE_G1,
    UI_NODE_LINK_NODE_G2,
    UI_NODE_LINK_NODE_G3,
    UI_NODE_LINK_NODE_G4,
} ui_node_link_node_slot_t;

extern const ui_node_link_node_slot_t ui_node_link_channel_node_map[4];

ui_node_link_node_slot_t ui_node_link_node_for_channel(uint8_t channel);
int8_t ui_node_link_channel_for_node(ui_node_link_node_slot_t node);
bool ui_node_link_status_changed(
    nrf_node_link_status_t previous,
    nrf_node_link_status_t current);
uint32_t ui_node_link_status_color(nrf_node_link_status_t status);
ui_i18n_key_t ui_node_link_status_label_key(nrf_node_link_status_t status);
