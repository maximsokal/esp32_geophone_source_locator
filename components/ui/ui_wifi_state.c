#include "ui_wifi_state.h"


static ui_wifi_page_t s_page =
    UI_WIFI_PAGE_MENU;

static size_t s_selection =
    0;


void ui_wifi_state_reset(void)
{
    s_page =
        UI_WIFI_PAGE_MENU;

    s_selection =
        0;
}


ui_wifi_page_t ui_wifi_state_page(void)
{
    return s_page;
}


void ui_wifi_state_set_page(
    ui_wifi_page_t page
)
{
    s_page =
        page;
}


size_t ui_wifi_state_selection(void)
{
    return s_selection;
}


void ui_wifi_state_set_selection(
    size_t selection
)
{
    if (
        selection <
        UI_WIFI_MENU_ITEM_COUNT
    ) {
        s_selection =
            selection;
    }
}
