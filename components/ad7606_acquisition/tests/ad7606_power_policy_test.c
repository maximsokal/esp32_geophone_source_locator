#include <assert.h>
#include <math.h>
#include <string.h>

#include "ad7606_power_policy.h"

/* MAX-242: AD7606 +5 V rail interlock policy. */

static const int64_t NOW_US = 10000000LL;

static void test_start_gate(void)
{
    /* Fresh valid readings decide. */
    assert(ad7606_power_gate(true, true, 5.02f, NOW_US - 500000, NOW_US) == AD7606_POWER_GATE_OK);
    assert(ad7606_power_gate(true, true, 4.75f, NOW_US - 1, NOW_US) == AD7606_POWER_GATE_OK);
    assert(ad7606_power_gate(true, true, 4.74f, NOW_US - 1, NOW_US) == AD7606_POWER_GATE_RAIL_LOW);
    assert(ad7606_power_gate(true, true, 0.02f, NOW_US - 1, NOW_US) == AD7606_POWER_GATE_RAIL_LOW);

    /* Anything that does not prove the rail state is "unverified", never OK
     * and never a false RAIL_LOW refusal. */
    assert(ad7606_power_gate(false, false, 0.0f, 0, NOW_US) == AD7606_POWER_GATE_UNVERIFIED);
    assert(ad7606_power_gate(true, false, 0.0f, NOW_US - 1, NOW_US) == AD7606_POWER_GATE_UNVERIFIED);
    assert(ad7606_power_gate(true, true, 5.0f, NOW_US - AD7606_RAIL_MAX_AGE_US - 1, NOW_US) == AD7606_POWER_GATE_UNVERIFIED);
    assert(ad7606_power_gate(true, true, 0.0f, NOW_US - AD7606_RAIL_MAX_AGE_US - 1, NOW_US) == AD7606_POWER_GATE_UNVERIFIED);
    assert(ad7606_power_gate(true, true, NAN, NOW_US - 1, NOW_US) == AD7606_POWER_GATE_UNVERIFIED);
    /* A timestamp from the future (clock confusion) proves nothing either. */
    assert(ad7606_power_gate(true, true, 5.0f, NOW_US + 1, NOW_US) == AD7606_POWER_GATE_UNVERIFIED);
}

static void test_rail_loss_requires_confirmation(void)
{
    ad7606_rail_watch_t watch;
    ad7606_rail_watch_reset(&watch);
    int64_t t = NOW_US;

    /* Good readings never trip. */
    for (int i = 0; i < 5; ++i) {
        t += 1000000;
        assert(!ad7606_rail_watch_update(&watch, true, true, 4.98f, t, t));
    }

    /* One low reading alone does not stop a recording. */
    t += 1000000;
    assert(!ad7606_rail_watch_update(&watch, true, true, 3.1f, t, t));
    /* The SAME conversion read again counts once. */
    assert(!ad7606_rail_watch_update(&watch, true, true, 3.1f, t, t + 10));
    /* An invalid or stale reading neither confirms nor clears the streak. */
    assert(!ad7606_rail_watch_update(&watch, true, false, 0.0f, t + 20, t + 20));
    assert(!ad7606_rail_watch_update(&watch, true, true, 0.0f, t - AD7606_RAIL_MAX_AGE_US - 5, t));
    /* The second distinct fresh low reading confirms the loss. */
    t += 1000000;
    assert(ad7606_rail_watch_update(&watch, true, true, 3.0f, t, t));
}

static void test_recovery_clears_the_streak_and_hysteresis_holds(void)
{
    ad7606_rail_watch_t watch;
    ad7606_rail_watch_reset(&watch);
    int64_t t = NOW_US;

    t += 1000000;
    assert(!ad7606_rail_watch_update(&watch, true, true, 4.2f, t, t));
    t += 1000000;
    assert(!ad7606_rail_watch_update(&watch, true, true, 4.9f, t, t)); /* recovered */
    t += 1000000;
    assert(!ad7606_rail_watch_update(&watch, true, true, 4.2f, t, t)); /* streak restarts */

    /* Between the loss (4.50) and start (4.75) thresholds a RUNNING AD7606
     * is not stopped, although it could not be started there. */
    ad7606_rail_watch_reset(&watch);
    for (int i = 0; i < 4; ++i) {
        t += 1000000;
        assert(!ad7606_rail_watch_update(&watch, true, true, 4.6f, t, t));
    }
    assert(ad7606_power_gate(true, true, 4.6f, t, t) == AD7606_POWER_GATE_RAIL_LOW);
}

static void test_names_and_null_safety(void)
{
    assert(strcmp(ad7606_power_gate_name(AD7606_POWER_GATE_OK), "ok") == 0);
    assert(strcmp(ad7606_power_gate_name(AD7606_POWER_GATE_UNVERIFIED), "unverified") == 0);
    assert(strcmp(ad7606_power_gate_name(AD7606_POWER_GATE_RAIL_LOW), "rail_low") == 0);
    assert(strcmp(ad7606_power_gate_name(AD7606_POWER_GATE_OVERCURRENT), "overcurrent") == 0);
    ad7606_rail_watch_reset(NULL);
    assert(!ad7606_rail_watch_update(NULL, true, true, 0.0f, NOW_US, NOW_US));
}

int main(void)
{
    test_start_gate();
    test_rail_loss_requires_confirmation();
    test_recovery_clears_the_streak_and_hysteresis_holds();
    test_names_and_null_safety();
    return 0;
}
