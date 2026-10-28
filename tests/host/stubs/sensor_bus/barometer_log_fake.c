#include "esp_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

char fake_barometer_log[8192];

/* Capture diagnostics so tests can distinguish reset data from other pending states. */
void fake_sensor_bus_log(const char *tag, const char *format, ...)
{
    (void)tag;
    const size_t used = strlen(fake_barometer_log);
    va_list args;
    va_start(args, format);
    (void)vsnprintf(fake_barometer_log + used, sizeof(fake_barometer_log) - used, format, args);
    va_end(args);
}

void fake_sensor_bus_log_debug(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}
