#ifndef GEOPHONE_RENDER_LV_CONF_H
#define GEOPHONE_RENDER_LV_CONF_H
#define LV_CONF_H

/* Match sdkconfig's renderer, depth, DPI and themes; replace only the OS/display. */
#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_RGB565
#define LV_USE_OS LV_OS_NONE
#define LV_MEM_SIZE (8 * 1024 * 1024)
#define LV_DPI_DEF 130
#define LV_USE_DRAW_SW 1
#define LV_DRAW_SW_COMPLEX 1
#define LV_USE_THORVG 0
#define LV_USE_TEST 1
#define LV_USE_TEST_SCREENSHOT_COMPARE 1
#define LV_TEST_SCREENSHOT_CREATE_REFERENCE_IMAGE 0
#define LV_USE_LODEPNG 1
#define LV_USE_FS_STDIO 1
#define LV_FS_STDIO_LETTER 'S'
#define LV_USE_SNAPSHOT 1
#define LV_USE_QRCODE 1
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1
#define LV_USE_SYSMON 0
#endif
