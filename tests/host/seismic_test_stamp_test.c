#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "seismic_test_stamp.h"

/*
 * Emulator scenario S10 failed on and off, with three faces of one defect in the synthetic source's timestamps:
 *   - "START rejected: acquisition sample is not fresh": the last stamp was 8.8 ms AHEAD of the clock;
 *   - the first write refused at once (written=0 in 186 us): a stamp stepped BACK, the V2 encoder rejects such a block
 *     (a lost "last_stamp_us = stamp_us" had switched the monotonic guard off);
 *   - "LOC_SEARCH ... ESP_ERR_INVALID_RESPONSE": stamps that were not one frame period apart (a fix with steps of 1 us), which
 *     the V2 file cannot store, so the reader rebuilt other times than the ones that were recorded.
 * The model below runs the generator's loop (generator.c): every iteration starts at some clock time, the tick waits until
 * the clock reaches its last stamp, then pushes. `clock_us` models esp_timer_get_time().
 */

typedef struct {
    int64_t next_stamp_us;
    int64_t last_stamp_us;
    int64_t clock_us;
    unsigned frames;
    unsigned future_frames;        /* a stamp later than the clock at the moment it was pushed */
    unsigned off_grid_frames;      /* not exactly one period after the previous stamp, without a re-anchor */
    unsigned reanchors;
    unsigned waits;
} model_t;

static void model_tick(model_t *m, int frames_per_tick, int64_t period_us, int64_t clock_at_wake_us)
{
    if (clock_at_wake_us > m->clock_us) m->clock_us = clock_at_wake_us;
    const int64_t first_us = seismic_test_stamp_first(
        m->next_stamp_us, frames_per_tick, period_us, m->clock_us, 4 * (int64_t)frames_per_tick * period_us);
    const int64_t ready_us = seismic_test_stamp_ready_at(first_us, frames_per_tick, period_us);
    if (m->clock_us < ready_us) {                          /* the generator waits: the clock moves on */
        m->clock_us = ready_us;
        ++m->waits;
    }
    const bool continues_grid = m->next_stamp_us != 0 && first_us == m->next_stamp_us;
    if (m->next_stamp_us != 0 && !continues_grid) ++m->reanchors;
    for (int k = 0; k < frames_per_tick; ++k) {
        const int64_t stamp_us = first_us + (int64_t)k * period_us;
        if (m->last_stamp_us != 0) {
            assert(stamp_us > m->last_stamp_us);           /* strictly increasing, always */
            if (stamp_us - m->last_stamp_us != period_us && !(k == 0 && !continues_grid)) ++m->off_grid_frames;
        }
        if (stamp_us > m->clock_us) ++m->future_frames;
        m->last_stamp_us = stamp_us;
        ++m->frames;
    }
    m->next_stamp_us = first_us + (int64_t)frames_per_tick * period_us;
}

static void test_exact_ticks_never_wait_and_stay_on_the_grid(void)
{
    model_t m = {0};
    for (int tick = 0; tick < 1000; ++tick) model_tick(&m, 5, 2000, 1000000 + (int64_t)tick * 10000);   /* 500 Hz, the board */
    assert(m.frames == 5000U);
    assert(m.future_frames == 0U && m.off_grid_frames == 0U);
    assert(m.waits == 0U && m.reanchors == 0U);
    assert(m.last_stamp_us == 1000000 + 999 * 10000);                  /* the last frame is the last tick's time */
}

static void test_the_first_tick_ends_at_the_clock(void)
{
    assert(seismic_test_stamp_first(0, 5, 2000, 50000, 40000) == 50000 - 4 * 2000);
    assert(seismic_test_stamp_ready_at(50000 - 4 * 2000, 5, 2000) == 50000);
    assert(seismic_test_stamp_first(0, 1, 2000, 10000, 8000) == 10000);           /* one frame per tick (non-scenario build) */
}

/* Emulator: ticks arrive after 4 ms of clock instead of 10 ms. The old rules ran ahead of the clock or off the grid. */
static void test_early_ticks_wait_for_the_clock_instead_of_running_ahead(void)
{
    model_t m = {0};
    int64_t wake = 14670000;
    for (int tick = 0; tick < 500; ++tick) {
        model_tick(&m, 5, 2000, wake);
        wake += 4000;
    }
    assert(m.frames == 2500U);
    assert(m.future_frames == 0U);                                                 /* never a sample from the future */
    assert(m.off_grid_frames == 0U);                                               /* and always one period apart */
    assert(m.waits > 400U);                                                        /* the throttle is what did it */
    assert(m.reanchors == 0U);
}

/* A stall longer than four ticks moves the grid to the clock; later ticks continue it. */
static void test_a_long_stall_moves_the_grid_forward_once(void)
{
    model_t m = {0};
    for (int tick = 0; tick < 100; ++tick) model_tick(&m, 5, 2000, 1000000 + (int64_t)tick * 10000);
    const int64_t before = m.last_stamp_us;
    model_tick(&m, 5, 2000, 1000000 + 100 * 10000 + 500000);                      /* 500 ms stall */
    assert(m.reanchors == 1U);
    assert(m.last_stamp_us > before + 400000);                                     /* the grid caught up with the clock */
    assert(m.last_stamp_us <= m.clock_us);
    const unsigned off_grid = m.off_grid_frames;
    for (int tick = 101; tick < 200; ++tick) model_tick(&m, 5, 2000, 1000000 + 500000 + (int64_t)tick * 10000);
    assert(m.off_grid_frames == off_grid && m.reanchors == 1U);
    assert(m.future_frames == 0U);
}

/* A late tick inside four ticks keeps the grid (the samples are older than the clock, never newer). */
static void test_a_slightly_late_tick_keeps_the_grid(void)
{
    model_t m = {0};
    model_tick(&m, 10, 1000, 5000000);
    model_tick(&m, 10, 1000, 5000000 + 25000);                                     /* 15 ms late at 1 kHz */
    model_tick(&m, 10, 1000, 5000000 + 30000);
    assert(m.reanchors == 0U && m.off_grid_frames == 0U && m.future_frames == 0U);
    assert(m.frames == 30U);
}

/* Random wake-ups, every rate the recorder offers: the three rules hold in all of them. */
static void test_random_jitter_keeps_every_rule_at_every_rate(void)
{
    static const int rates[] = {500, 1000, 2000, 4000, 5000};
    for (size_t r = 0U; r < sizeof(rates) / sizeof(rates[0]); ++r) {
        const int frames_per_tick = rates[r] * 10 / 1000;
        const int64_t period_us = 1000000 / rates[r];
        model_t m = {0};
        int64_t wake = 20000000;
        uint32_t seed = 777U + (uint32_t)r;
        for (int tick = 0; tick < 3000; ++tick) {
            seed = seed * 1664525U + 1013904223U;
            wake += 500 + (int64_t)(seed >> 16) % 25000;                           /* 0.5 .. 25.5 ms between wake-ups */
            model_tick(&m, frames_per_tick, period_us, wake);
        }
        assert(m.future_frames == 0U);
        assert(m.off_grid_frames == 0U);
        assert(m.frames == (unsigned)(3000 * frames_per_tick));
    }
}

int main(void)
{
    test_exact_ticks_never_wait_and_stay_on_the_grid();
    test_the_first_tick_ends_at_the_clock();
    test_early_ticks_wait_for_the_clock_instead_of_running_ahead();
    test_a_long_stall_moves_the_grid_forward_once();
    test_a_slightly_late_tick_keeps_the_grid();
    test_random_jitter_keeps_every_rule_at_every_rate();
    puts("seismic test stamp host contract passed");
    return 0;
}
