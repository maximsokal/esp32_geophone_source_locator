#include "ui_internal.h"

#include <stdio.h>

#include "device_status.h"
#include "display.h"
#include "ui_theme.h"


esp_err_t ui_status_bar_draw(void)
{
    device_status_snapshot_t status;


    ESP_ERROR_CHECK(
        device_status_get_snapshot(
            &status
        )
    );


    char power_line[64];


    if (status.power.valid) {

        snprintf(
            power_line,
            sizeof(power_line),
            "PWR %.2fV %+.2fA  MODE %s",
            status.power.voltage_v,
            status.power.current_a,
            device_status_mode_name(
                status.mode
            )
        );
    }
    else {

        snprintf(
            power_line,
            sizeof(power_line),
            "PWR --.--V --.--A  MODE %s",
            device_status_mode_name(
                status.mode
            )
        );
    }


    char pods_line[64];


    snprintf(
        pods_line,
        sizeof(pods_line),
        "PODS 1:%s  2:%s  3:%s  4:%s",
        device_status_pod_short_name(
            status.pods[0].state
        ),
        device_status_pod_short_name(
            status.pods[1].state
        ),
        device_status_pod_short_name(
            status.pods[2].state
        ),
        device_status_pod_short_name(
            status.pods[3].state
        )
    );


    ESP_ERROR_CHECK(
        display_fill_rect(
            0,
            0,
            DISPLAY_WIDTH,
            UI_STATUS_BAR_HEIGHT,
            UI_COLOR_STATUS_BG
        )
    );


    ESP_ERROR_CHECK(
        display_draw_text(
            6,
            3,
            power_line,
            UI_COLOR_TEXT,
            UI_COLOR_STATUS_BG,
            1
        )
    );


    ESP_ERROR_CHECK(
        display_draw_text(
            6,
            18,
            pods_line,
            UI_COLOR_SECONDARY,
            UI_COLOR_STATUS_BG,
            1
        )
    );


    return ESP_OK;
}