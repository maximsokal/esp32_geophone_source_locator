#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "ui_node_link_status.h"
#include "ui_theme.h"

static void test_status_widget_changes_only_when_status_changes(void)
{
    assert(!ui_node_link_status_changed(
        NRF_NODE_LINK_STATUS_READY,
        NRF_NODE_LINK_STATUS_READY));
    assert(ui_node_link_status_changed(
        NRF_NODE_LINK_STATUS_READY,
        NRF_NODE_LINK_STATUS_MEASURING));
}

static void test_status_colors_and_text_keys(void)
{
    assert(ui_node_link_status_color(NRF_NODE_LINK_STATUS_NOT_CONNECTED) == UI_COLOR_ERROR);
    assert(ui_node_link_status_color(NRF_NODE_LINK_STATUS_CONNECTED) == UI_COLOR_WARNING);
    assert(ui_node_link_status_color(NRF_NODE_LINK_STATUS_READY) == UI_COLOR_GOOD);
    assert(ui_node_link_status_color(NRF_NODE_LINK_STATUS_MEASURING) == UI_COLOR_LINK_MEASURING);

    assert(ui_node_link_status_label_key(NRF_NODE_LINK_STATUS_NOT_CONNECTED) ==
        UI_I18N_KEY_MEASUREMENT_LINK_NOT_CONNECTED);
    assert(ui_node_link_status_label_key(NRF_NODE_LINK_STATUS_CONNECTED) ==
        UI_I18N_KEY_MEASUREMENT_LINK_CONNECTED);
    assert(ui_node_link_status_label_key(NRF_NODE_LINK_STATUS_READY) ==
        UI_I18N_KEY_MEASUREMENT_LINK_READY);
    assert(ui_node_link_status_label_key(NRF_NODE_LINK_STATUS_MEASURING) ==
        UI_I18N_KEY_MEASUREMENT_LINK_MEASURING);
}

static void test_channel_to_node_map_and_g4_unmapped(void)
{
    assert(ui_node_link_channel_node_map[0] == UI_NODE_LINK_NODE_MAIN);
    assert(ui_node_link_channel_node_map[1] == UI_NODE_LINK_NODE_G1);
    assert(ui_node_link_channel_node_map[2] == UI_NODE_LINK_NODE_G2);
    assert(ui_node_link_channel_node_map[3] == UI_NODE_LINK_NODE_G3);
    assert(ui_node_link_channel_for_node(UI_NODE_LINK_NODE_MAIN) == 0);
    assert(ui_node_link_channel_for_node(UI_NODE_LINK_NODE_G1) == 1);
    assert(ui_node_link_channel_for_node(UI_NODE_LINK_NODE_G2) == 2);
    assert(ui_node_link_channel_for_node(UI_NODE_LINK_NODE_G3) == 3);
    assert(ui_node_link_channel_for_node(UI_NODE_LINK_NODE_G4) == -1);
}

int main(void)
{
    test_status_widget_changes_only_when_status_changes();
    test_status_colors_and_text_keys();
    test_channel_to_node_map_and_g4_unmapped();
    puts("ui_node_link_status_test: PASS");
    return 0;
}
