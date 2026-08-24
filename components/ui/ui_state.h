#pragma once

#include <stddef.h>

#include "ui_screen.h"


void ui_state_reset(void);


ui_screen_t ui_state_screen(void);


void ui_state_set_screen(
    ui_screen_t screen
);


size_t ui_state_home_selection(void);


void ui_state_set_home_selection(
    size_t selected_item
);
