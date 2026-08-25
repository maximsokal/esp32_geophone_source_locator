#pragma once

#include <stdbool.h>

#include "esp_err.h"


esp_err_t sd_card_mount(void);


bool sd_card_is_mounted(void);


void sd_card_unmount(void);
