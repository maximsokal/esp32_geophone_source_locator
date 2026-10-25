#include <assert.h>
#include "i2c_timeout_budget.h"

int main(void)
{
    assert(i2c_timeout_remaining_ticks(100U, 2U, 100U) == 2U);
    assert(i2c_timeout_remaining_ticks(100U, 2U, 101U) == 1U);
    assert(i2c_timeout_remaining_ticks(100U, 2U, 102U) == 0U);
    assert(i2c_timeout_remaining_ticks(100U, 2U, 103U) == 0U);
    assert(i2c_timeout_remaining_ticks(UINT32_MAX - 1U, 4U, 1U) == 1U);
    assert(i2c_timeout_remaining_ticks(UINT32_MAX - 1U, 4U, 2U) == 0U);
    assert(i2c_timeout_remaining_ticks(10U, portMAX_DELAY, 300U) == portMAX_DELAY);
    assert(i2c_timeout_remaining_ticks(10U, 0U, 10U) == 0U);

    /* Each FIFO command consumes the original deadline, including final completion. */
    TickType_t now = 100U;
    for (unsigned command = 0U; command < 5U; ++command) {
        const TickType_t remaining = i2c_timeout_remaining_ticks(100U, 2U, now);
        if (remaining == 0U) break;
        ++now;
    }
    assert(now == 102U);
    assert(i2c_timeout_remaining_ticks(100U, 2U, now) == 0U);
    return 0;
}
