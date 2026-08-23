#pragma once

#include <stdbool.h>

#include "esp_err.h"


esp_err_t captive_dns_server_start(void);

void captive_dns_server_stop(void);

bool captive_dns_server_is_running(void);