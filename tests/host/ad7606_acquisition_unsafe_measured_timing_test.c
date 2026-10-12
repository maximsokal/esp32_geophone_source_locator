#include <assert.h>
#include <stdio.h>

#include "ad7606_acquisition.h"

bool ad7606_acquisition_test_start_with_tconv(uint32_t tconv_ns);

int main(void)
{
    /* 36 us classifies as OS8; OS8 cannot meet 40 kHz, while configured OS1 can. */
    assert(!ad7606_acquisition_test_start_with_tconv(36000U));

    ad7606_acquisition_status_t status;
    assert(ad7606_acquisition_get_status(&status) == ESP_OK);
    assert(status.os_ratio_configured == 1U);
    assert(status.os_ratio_measured == 8U);
    assert(status.os_mismatch);

    puts("MAX435 T7 PASS: unsafe measured OS8 timing blocks configured OS1 start");
    return 0;
}
