#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "seismic_test_stamp.h"

/*
 * Emulator scenario S10 failed on and off with two different faces of the same defect in the synthetic source's
 * timestamps:
 *   - "START rejected: acquisition sample is not fresh" (the last stamp was 8.8 ms AHEAD of the clock), and
 *   - the first write failing at once, written=0 in 186 us (a stamp stepped BACK, the V2 encoder refuses that block).
 * The first came from adding a whole frame period to the previous stamp when ticks arrived early; the second from my own
 * edit deleting "last_stamp_us = stamp_us", which switched the monotonic guard off. These tests drive the same loop the
 * generator runs, with the clock behaving like a board (exact ticks) and like the emulator (early/late ticks).
 */

typedef struct {
    int64_t last_stamp_us;
    int64_t previous_clock_us;
    unsigned frames;
    unsigned future_frames;       /* a stamp later than the clock at the moment it was made */
    int64_t worst_lead_us;
} generator_t;

/* One tick of the generator's loop: frames_per_tick frames stamped from tick_us, the clock read per frame. */
static void run_tick(generator_t *g, int64_t tick_us, int frames_per_tick, int64_t frame_period_us, int64_t clock_us)
{
    for (int k = 0; k < frames_per_tick; ++k) {
        const int64_t stamp_us =
            seismic_test_stamp_next(tick_us, frames_per_tick, k, frame_period_us, g->last_stamp_us, clock_us);
        if (g->last_stamp_us != 0) assert(stamp_us > g->last_stamp_us);        /* strictly increasing, always */
        if (stamp_us > clock_us) {
            ++g->future_frames;
            if (stamp_us - clock_us > g->worst_lead_us) g->worst_lead_us = stamp_us - clock_us;
        }
        g->last_stamp_us = stamp_us;
        ++g->frames;
    }
}

static void test_exact_ticks_stamp_backwards_from_the_tick_and_never_run_ahead(void)
{
    generator_t g = {0};
    /* 500 Hz, 10 ms ticks, 5 frames per tick: the board. */
    for (int tick = 0; tick < 1000; ++tick) {
        const int64_t t = 1000000 + (int64_t)tick * 10000;
        run_tick(&g, t, 5, 2000, t);
    }
    assert(g.frames == 5000U && g.future_frames == 0U);
    assert(g.last_stamp_us == 1000000 + 999 * 10000);                              /* the last frame is the last tick */
}

static void test_the_first_frame_is_stamped_from_its_tick(void)
{
    assert(seismic_test_stamp_next(50000, 5, 0, 2000, 0, 50000) == 50000 - 4 * 2000);
    assert(seismic_test_stamp_next(50000, 5, 4, 2000, 0, 50000) == 50000);
    assert(seismic_test_stamp_next(10000, 1, 0, 2000, 0, 10000) == 10000);         /* one frame per tick (non-scenario build) */
}

/* Emulator face 1: ticks closer together than the clock moves. The old code put the stamps a whole period ahead. */
static void test_ticks_that_come_early_do_not_run_ahead_of_the_clock(void)
{
    generator_t g = {0};
    const int64_t period = 2000;
    int64_t clock = 14670000;
    run_tick(&g, clock, 5, period, clock);                                         /* first tick, exact */
    for (int tick = 0; tick < 200; ++tick) {
        clock += 4000;                                                             /* the tick fires after 4 ms of clock, not 10 */
        run_tick(&g, clock, 5, period, clock);
    }
    /* 10 ms of frames per 4 ms of clock cannot be both strictly increasing and never ahead; the lead is microseconds
     * (1 us per frame that has no room) instead of the milliseconds the recorder rejected. */
    assert(g.worst_lead_us < 1000);
    /* The old rule on the very same sequence: the lead the recorder rejected. */
    int64_t last = 0, worst = 0, c = 14670000;
    for (int tick = 0; tick < 201; ++tick) {
        for (int k = 0; k < 5; ++k) {
            int64_t stamp = c - (int64_t)(4 - k) * period;
            if (last != 0 && stamp <= last) stamp = last + period;
            if (stamp - c > worst) worst = stamp - c;
            last = stamp;
        }
        c += 4000;
    }
    assert(worst > 8000);                                                          /* 8.8 ms in the bench log, here more */
    assert(g.worst_lead_us * 8 < worst);                                           /* the new rule: an order of magnitude less */
}

/* Emulator face 2 / the dead guard: a tick that is LATE or jittery must not make stamps step back. */
static void test_jittery_ticks_never_step_back(void)
{
    generator_t g = {0};
    const int64_t period = 1000;                                                   /* 1 kHz, 10 frames per tick */
    int64_t clock = 5000000;
    uint32_t seed = 12345U;
    for (int tick = 0; tick < 5000; ++tick) {
        seed = seed * 1664525U + 1013904223U;
        const int64_t delta = 500 + (int64_t)(seed >> 16) % 20000;                 /* 0.5 .. 20.5 ms between ticks */
        clock += delta;
        run_tick(&g, clock, 10, period, clock);                                    /* asserts strict increase every frame */
    }
    assert(g.frames == 50000U);
}

/* The clock itself stalls (or reads equal): strictly increasing still wins, by 1 us, and the lead stays tiny. */
static void test_a_stalled_clock_still_gives_strictly_increasing_stamps(void)
{
    generator_t g = {0};
    run_tick(&g, 1000000, 5, 2000, 1000000);
    for (int tick = 0; tick < 50; ++tick) run_tick(&g, 1000000, 5, 2000, 1000000);
    assert(g.frames == 255U);
    assert(g.last_stamp_us == 1000000 + 250);                                      /* 250 frames after the first tick, 1 us each */
    assert(g.worst_lead_us <= 250);
}

/* The guard really is stateful: dropping the update of the previous stamp (the deleted line) must be caught. */
static void test_the_previous_stamp_is_what_keeps_the_trace_monotonic(void)
{
    /* A tick whose backwards run starts before the previous stamp: 5 frames from t=1,000,000 (stamps 992000..1000000),
     * then a tick 3 ms later at the same 2 ms period (stamps 995000..1003000 would overlap). */
    int64_t last = 1000000;
    const int64_t first = seismic_test_stamp_next(1003000, 5, 0, 2000, last, 1003000);
    assert(first > last);                                                          /* not 995000 */
    assert(first <= 1003000);                                                      /* and not ahead of the clock */
    /* With no previous stamp known (last == 0) the same call is the plain backwards stamp: the failure mode. */
    assert(seismic_test_stamp_next(1003000, 5, 0, 2000, 0, 1003000) == 995000);
}

/* The clock may be far past the tick (a late read): the step is still one frame period, the rate does not jump. */
static void test_the_step_never_exceeds_one_frame_period(void)
{
    const int64_t first = seismic_test_stamp_next(1000000, 5, 0, 2000, 999500, 1050000);   /* raw 992000 is behind 999500 */
    assert(first == 999500 + 2000);
    /* Not enough room: the step shrinks to the room, not to a full period. */
    assert(seismic_test_stamp_next(1000000, 5, 0, 2000, 999500, 1000200) == 999500 + 700);
}

int main(void)
{
    test_the_step_never_exceeds_one_frame_period();
    test_exact_ticks_stamp_backwards_from_the_tick_and_never_run_ahead();
    test_the_first_frame_is_stamped_from_its_tick();
    test_ticks_that_come_early_do_not_run_ahead_of_the_clock();
    test_jittery_ticks_never_step_back();
    test_a_stalled_clock_still_gives_strictly_increasing_stamps();
    test_the_previous_stamp_is_what_keeps_the_trace_monotonic();
    puts("seismic test stamp host contract passed");
    return 0;
}
