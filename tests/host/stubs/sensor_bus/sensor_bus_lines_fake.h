#pragma once

#include <stdbool.h>
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

    /* Observations. */
    int bitbang_prepare_calls;
    int controller_reset_calls;
    int scl_driven_low;
    int rising_edges;
    int64_t now_us;

    /* Internal controller-side drive (bit-bang); zero state = healthy idle bus. */
    bool ctrl_sda_low;
    bool ctrl_scl_low;
    bool last_scl_low;

} fake_lines_t;

extern fake_lines_t g_fake_lines;

void fake_lines_reset(void);
