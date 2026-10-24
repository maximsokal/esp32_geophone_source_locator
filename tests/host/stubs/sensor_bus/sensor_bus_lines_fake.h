#pragma once

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

/* MAX-243: line model behind sensor_bus_lines.h for host tests. */
typedef struct
{
    /* Target behaviour. SDA is held low until `sda_hold_clocks` rising SCL
     * edges were seen (bit-bang) or a controller reset happens (live);
     * -1 = forever, 0 = not holding. */
    int sda_hold_clocks;
    bool scl_held;
    bool controller_reset_clears_sda;
    bool controller_reset_fails;

    /* Observations. */
    int bitbang_prepare_calls;
    int controller_reset_calls;
    int scl_driven_low;
    int rising_edges;
    atomic_int_fast64_t now_us;
    unsigned log_calls;
    char last_log[512];
    char timeout_config_log[512];
    char init_lines_log[512];
    bool hold_budget_exceeded_logged;

    /* MAX-248 address probe: result per 7-bit address (default ESP_OK is
     * overridden by probe_default when probe_configured is false). */
    bool probe_configured;
    int probe_result[128];
    int probe_calls;

    /* Internal controller-side drive (bit-bang); zero state = healthy idle bus. */
    bool ctrl_sda_low;
    bool ctrl_scl_low;
    bool last_scl_low;

} fake_lines_t;

extern fake_lines_t g_fake_lines;

void fake_lines_reset(void);
void fake_sensor_bus_log(const char *tag, const char *format, ...);
