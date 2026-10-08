#include <assert.h>
#include <stdint.h>

#include "storage_manager_slow_op_policy.h"


static unsigned s_warn_count;


static void count_if_warned(
    storage_manager_slow_op_state_t *state,
    size_t source_id,
    uint32_t now_ms,
    uint32_t elapsed_ms
)
{
    if (
        storage_manager_slow_op_should_warn(
            state,
            source_id,
            now_ms,
            elapsed_ms
        )
    ) {
        ++s_warn_count;
    }
}


static void test_rate_limit_and_first_event_at_zero(void)
{
    storage_manager_slow_op_state_t state = {0};
    s_warn_count = 0U;

    count_if_warned(&state, 0U, 0U, 501U);
    assert(s_warn_count == 1U);

    count_if_warned(&state, 0U, 4999U, 501U);
    assert(s_warn_count == 1U);

    count_if_warned(&state, 0U, 5000U, 501U);
    assert(s_warn_count == 2U);
}


static void test_elapsed_threshold_is_strict(void)
{
    storage_manager_slow_op_state_t state = {0};
    s_warn_count = 0U;

    assert(storage_manager_slow_op_elapsed_ms(500000LL) == 500U);
    assert(storage_manager_slow_op_elapsed_ms(500001LL) == 501U);
    count_if_warned(&state, 0U, 10U, 500U);
    assert(s_warn_count == 0U);

    count_if_warned(&state, 0U, 10U, 501U);
    assert(s_warn_count == 1U);
}


static void test_sources_are_rate_limited_independently(void)
{
    storage_manager_slow_op_state_t state = {0};
    s_warn_count = 0U;

    count_if_warned(&state, 0U, 100U, 501U);
    count_if_warned(&state, 1U, 100U, 501U);
    assert(s_warn_count == 2U);
}


static void test_rate_limit_handles_uint32_wraparound(void)
{
    storage_manager_slow_op_state_t state = {0};
    s_warn_count = 0U;

    count_if_warned(&state, 0U, UINT32_MAX - 2000U, 501U);
    assert(s_warn_count == 1U);

    count_if_warned(&state, 0U, 2998U, 501U);
    assert(s_warn_count == 1U);

    count_if_warned(&state, 0U, 2999U, 501U);
    assert(s_warn_count == 2U);
}


int main(void)
{
    test_rate_limit_and_first_event_at_zero();
    test_elapsed_threshold_is_strict();
    test_sources_are_rate_limited_independently();
    test_rate_limit_handles_uint32_wraparound();
    return 0;
}
