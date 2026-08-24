#pragma once


#define UI_MENU_ITEM_COUNT 5


typedef enum
{
    UI_SCREEN_HOME = 0,
    UI_SCREEN_OBJECTS,
    UI_SCREEN_MEASUREMENTS,
    UI_SCREEN_STORAGE,
    UI_SCREEN_WIFI,
    UI_SCREEN_SYSTEM,

    UI_SCREEN_COUNT

} ui_screen_t;


const char *ui_screen_name(
    ui_screen_t screen
);
