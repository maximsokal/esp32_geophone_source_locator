#include "ui_state.h"


typedef struct
{
    ui_screen_t screen;

    size_t home_selection;

} ui_state_data_t;


static ui_state_data_t s_state = {
    .screen =
        UI_SCREEN_HOME,

    .home_selection =
        0,
};


void ui_state_reset(void)
{
    s_state.screen =
        UI_SCREEN_HOME;

    s_state.home_selection =
        0;
}


ui_screen_t ui_state_screen(void)
{
    return
        s_state.screen;
}


void ui_state_set_screen(
    ui_screen_t screen
)
{
    s_state.screen =
        screen;
}


size_t ui_state_home_selection(void)
{
    return
        s_state.home_selection;
}


void ui_state_set_home_selection(
    size_t selected_item
)
{
    s_state.home_selection =
        selected_item;
}
