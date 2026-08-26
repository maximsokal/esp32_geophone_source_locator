#pragma once

#include <stddef.h>


#define UI_WIFI_MENU_ITEM_COUNT    6


typedef enum
{
    UI_WIFI_PAGE_MENU = 0,
    UI_WIFI_PAGE_QUICK_CONFIRM,
    UI_WIFI_PAGE_CONNECT_SETUP,
    UI_WIFI_PAGE_OPEN_SETUP,
    UI_WIFI_PAGE_ENTER_CREDENTIALS,
    UI_WIFI_PAGE_WAIT_HOTSPOT,
    UI_WIFI_PAGE_OPEN_DEVICE,
    UI_WIFI_PAGE_FORGET_CONFIRM,

} ui_wifi_page_t;


void ui_wifi_state_reset(void);

ui_wifi_page_t ui_wifi_state_page(void);

void ui_wifi_state_set_page(
    ui_wifi_page_t page
);

size_t ui_wifi_state_selection(void);

void ui_wifi_state_set_selection(
    size_t selection
);
