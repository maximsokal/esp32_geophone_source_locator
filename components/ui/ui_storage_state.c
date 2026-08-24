#include "ui_storage_internal.h"


static ui_storage_state_t s_state = {
    .mode =
        STORAGE_MODE_MENU,

    .requested_mode =
        STORAGE_MODE_CARD_INFO,

    .selected_action =
        0,

    .notice = "",

    .notice_warning =
        false,
};


ui_storage_state_t *ui_storage_state(void)
{
    return
        &s_state;
}


void ui_storage_state_reset(void)
{
    s_state.mode =
        STORAGE_MODE_MENU;

    s_state.requested_mode =
        STORAGE_MODE_CARD_INFO;

    s_state.selected_action =
        0;

    s_state.notice[0] =
        '\0';

    s_state.notice_warning =
        false;
}
