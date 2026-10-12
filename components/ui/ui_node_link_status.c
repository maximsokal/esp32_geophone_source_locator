#include "ui_node_link_status.h"

#include "ui_theme.h"

const ui_node_link_node_slot_t ui_node_link_channel_node_map[4] = {
    UI_NODE_LINK_NODE_MAIN,
    UI_NODE_LINK_NODE_G1,
    UI_NODE_LINK_NODE_G2,
    UI_NODE_LINK_NODE_G3,
};

ui_node_link_node_slot_t ui_node_link_node_for_channel(uint8_t channel)
{
    return channel < 4U
        ? ui_node_link_channel_node_map[channel]
        : UI_NODE_LINK_NODE_G4;
}

int8_t ui_node_link_channel_for_node(ui_node_link_node_slot_t node)
{
    for (uint8_t channel = 0U; channel < 4U; ++channel) {
        if (ui_node_link_channel_node_map[channel] == node) {
            return (int8_t)channel;
        }
    }
    return -1;
}

bool ui_node_link_status_changed(
    nrf_node_link_status_t previous,
    nrf_node_link_status_t current)
{
    return previous != current;
}

uint32_t ui_node_link_status_color(nrf_node_link_status_t status)
{
    switch (status) {
    case NRF_NODE_LINK_STATUS_CONNECTED:
        return UI_COLOR_WARNING;
    case NRF_NODE_LINK_STATUS_READY:
        return UI_COLOR_GOOD;
    case NRF_NODE_LINK_STATUS_MEASURING:
        return UI_COLOR_LINK_MEASURING;
    case NRF_NODE_LINK_STATUS_NOT_CONNECTED:
    default:
        return UI_COLOR_ERROR;
    }
}

ui_i18n_key_t ui_node_link_status_label_key(nrf_node_link_status_t status)
{
    switch (status) {
    case NRF_NODE_LINK_STATUS_CONNECTED:
        return UI_I18N_KEY_MEASUREMENT_LINK_CONNECTED;
    case NRF_NODE_LINK_STATUS_READY:
        return UI_I18N_KEY_MEASUREMENT_LINK_READY;
    case NRF_NODE_LINK_STATUS_MEASURING:
        return UI_I18N_KEY_MEASUREMENT_LINK_MEASURING;
    case NRF_NODE_LINK_STATUS_NOT_CONNECTED:
    default:
        return UI_I18N_KEY_MEASUREMENT_LINK_NOT_CONNECTED;
    }
}
