/* The sample-rate side of the acquisition facade for host tests of web_status_json.c: the rate the next run would use. */
#include <stdbool.h>
#include <stdint.h>

uint32_t g_fake_selected_sample_rate_hz = 500U;

uint32_t seismic_acquisition_get_sample_rate(void)
{
    return g_fake_selected_sample_rate_hz;
}

bool seismic_acquisition_sample_rate_supported(uint32_t sample_rate_hz)
{
    return sample_rate_hz == 500U || sample_rate_hz == 2000U || sample_rate_hz == 4000U || sample_rate_hz == 5000U;
}
