#include "esp_log.h"

/* The barometer logs what each probed address did; its host tests only need the calls to link. */
void fake_sensor_bus_log(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}

void fake_sensor_bus_log_debug(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}
