#include <assert.h>
#include <string.h>

#include "ranging_radio_pause_core.h"

typedef struct {
    uint64_t now;
    bool running;
    int stop_result;
    int start_result;
    unsigned fail_starts;       /* refuse this many starts first */
    unsigned stop_calls;
    unsigned start_calls;
} fake_t;

static bool fake_running(void *c) { return ((fake_t *)c)->running; }
static uint64_t fake_now(void *c) { return ((fake_t *)c)->now; }
static int fake_stop(void *c)
{
    fake_t *f = c;
    ++f->stop_calls;
    if (f->stop_result == 0) f->running = false;
    return f->stop_result;
}
static int fake_start(void *c)
{
    fake_t *f = c;
    ++f->start_calls;
    if (f->fail_starts > 0U) { --f->fail_starts; return -1; }
    if (f->start_result == 0) f->running = true;
    return f->start_result;
}

static void setup(ranging_radio_core_t *core, fake_t *f)
{
    const ranging_radio_ops_t ops = {fake_running, fake_stop, fake_start, fake_now, f};
    memset(f, 0, sizeof(*f));
    f->running = true;
    f->now = 1000U;
    ranging_radio_core_init(core, &ops, 3000U, 5000U, 4U);
}

static void advance(ranging_radio_core_t *core, fake_t *f, uint32_t ms)
{
    f->now += ms;
    ranging_radio_core_step(core);
}

/* The whole round: announced, still on during the grace time, off, measurement may start, back on by itself. */
static void test_the_radio_goes_off_after_the_grace_time_and_comes_back_by_itself(void)
{
    ranging_radio_core_t core;
    fake_t f;

    setup(&core, &f);
    assert(core.state == RANGING_RADIO_IDLE && !ranging_radio_core_ready(&core));
    ranging_radio_core_request_off(&core);
    assert(core.state == RANGING_RADIO_PENDING && f.running && !ranging_radio_core_ready(&core));
    advance(&core, &f, 2999U);                      /* the screen and the web page still have time to say it */
    assert(core.state == RANGING_RADIO_PENDING && f.running && f.stop_calls == 0U);
    advance(&core, &f, 1U);
    assert(core.state == RANGING_RADIO_OFF && !f.running && f.stop_calls == 1U);
    assert(ranging_radio_core_ready(&core) && core.we_stopped && !core.stop_failed);

    ranging_radio_core_request_off(&core);          /* asked again (a retry): nothing happens twice */
    advance(&core, &f, 100000U);
    assert(core.state == RANGING_RADIO_OFF && f.stop_calls == 1U && !f.running);

    ranging_radio_core_request_on(&core);
    assert(core.state == RANGING_RADIO_RESUMING && !ranging_radio_core_ready(&core));
    ranging_radio_core_step(&core);
    assert(core.state == RANGING_RADIO_IDLE && f.running && f.start_calls == 1U && !core.we_stopped);
    assert(core.stops == 1U && core.starts == 1U && !core.resume_failed);
    ranging_radio_core_request_on(&core);           /* and idempotent */
    advance(&core, &f, 10000U);
    assert(f.start_calls == 1U && f.stop_calls == 1U);
}

/* Leaving the page during the grace time: the radio was never touched. */
static void test_leaving_before_the_radio_went_off_changes_nothing(void)
{
    ranging_radio_core_t core;
    fake_t f;

    setup(&core, &f);
    ranging_radio_core_request_off(&core);
    advance(&core, &f, 1000U);
    ranging_radio_core_request_on(&core);
    assert(core.state == RANGING_RADIO_IDLE && f.running);
    advance(&core, &f, 60000U);
    assert(f.stop_calls == 0U && f.start_calls == 0U && f.running);
}

/* A radio that is not running (nobody started it, or it is off for another reason) is not started by us afterwards. */
static void test_a_radio_that_was_off_already_is_not_started_afterwards(void)
{
    ranging_radio_core_t core;
    fake_t f;

    setup(&core, &f);
    f.running = false;
    ranging_radio_core_request_off(&core);
    advance(&core, &f, 3000U);
    assert(core.state == RANGING_RADIO_OFF && ranging_radio_core_ready(&core) && f.stop_calls == 0U && !core.we_stopped);
    ranging_radio_core_request_on(&core);
    ranging_radio_core_step(&core);
    assert(core.state == RANGING_RADIO_IDLE && f.start_calls == 0U && !f.running);
}

/* The stop is refused: the measurement is not held back, the radio stays on, and nothing is started afterwards. */
static void test_a_refused_stop_does_not_hold_the_measurement_back(void)
{
    ranging_radio_core_t core;
    fake_t f;

    setup(&core, &f);
    f.stop_result = -1;
    ranging_radio_core_request_off(&core);
    advance(&core, &f, 3000U);
    assert(core.state == RANGING_RADIO_OFF && ranging_radio_core_ready(&core) && core.stop_failed && f.running);
    ranging_radio_core_request_on(&core);
    ranging_radio_core_step(&core);
    assert(core.state == RANGING_RADIO_IDLE && f.start_calls == 0U && f.running);
    ranging_radio_core_request_off(&core);          /* a new round clears the old failure */
    assert(!core.stop_failed);
}

/* The start is refused (the Wi-Fi manager is busy or unsure of the radio): retried every 5 s, then given up with the reason kept. */
static void test_a_refused_start_is_retried_and_then_reported(void)
{
    ranging_radio_core_t core;
    fake_t f;

    setup(&core, &f);
    ranging_radio_core_request_off(&core);
    advance(&core, &f, 3000U);
    ranging_radio_core_request_on(&core);
    f.fail_starts = 2U;
    ranging_radio_core_step(&core);
    assert(core.state == RANGING_RADIO_RESUMING && f.start_calls == 1U);
    advance(&core, &f, 4999U);
    assert(f.start_calls == 1U);                    /* not before the retry time */
    advance(&core, &f, 1U);
    assert(core.state == RANGING_RADIO_RESUMING && f.start_calls == 2U);
    advance(&core, &f, 5000U);
    assert(core.state == RANGING_RADIO_IDLE && f.start_calls == 3U && f.running && !core.resume_failed);

    /* Never accepted: four tries, then the state is idle again with resume_failed set. */
    setup(&core, &f);
    ranging_radio_core_request_off(&core);
    advance(&core, &f, 3000U);
    ranging_radio_core_request_on(&core);
    f.start_result = -1;
    for (unsigned i = 0U; i < 4U; ++i) advance(&core, &f, 5000U);
    assert(core.state == RANGING_RADIO_IDLE && f.start_calls == 4U && core.resume_failed && !f.running);
    advance(&core, &f, 60000U);
    assert(f.start_calls == 4U);                    /* it does not go on for ever */
}

/* A second measurement is asked for while the radio is coming back: it finishes coming back, then goes off again. */
static void test_a_new_measurement_during_the_return_waits_for_it(void)
{
    ranging_radio_core_t core;
    fake_t f;

    setup(&core, &f);
    ranging_radio_core_request_off(&core);
    advance(&core, &f, 3000U);
    ranging_radio_core_request_on(&core);
    f.fail_starts = 1U;
    ranging_radio_core_step(&core);                 /* refused once: RESUMING */
    ranging_radio_core_request_off(&core);
    assert(core.state == RANGING_RADIO_RESUMING && !ranging_radio_core_ready(&core));
    advance(&core, &f, 5000U);                      /* the retry succeeds */
    assert(f.running && core.state == RANGING_RADIO_PENDING);
    advance(&core, &f, 3000U);
    assert(core.state == RANGING_RADIO_OFF && !f.running && f.stop_calls == 2U);
}

static void test_names_and_bad_arguments(void)
{
    ranging_radio_core_t core;
    fake_t f;

    assert(strcmp(ranging_radio_state_name(RANGING_RADIO_IDLE), "idle") == 0);
    assert(strcmp(ranging_radio_state_name(RANGING_RADIO_PENDING), "pending") == 0);
    assert(strcmp(ranging_radio_state_name(RANGING_RADIO_OFF), "off") == 0);
    assert(strcmp(ranging_radio_state_name(RANGING_RADIO_RESUMING), "resuming") == 0);
    assert(strcmp(ranging_radio_state_name((ranging_radio_state_t)9), "?") == 0);
    ranging_radio_core_request_off(NULL);
    ranging_radio_core_request_on(NULL);
    ranging_radio_core_step(NULL);
    assert(!ranging_radio_core_ready(NULL));
    setup(&core, &f);
    ranging_radio_core_init(&core, NULL, 1U, 1U, 1U);                 /* ignored */
    assert(core.grace_ms == 3000U);
    const ranging_radio_ops_t no_ops = {0};
    ranging_radio_core_init(&core, &no_ops, 10U, 10U, 0U);              /* an empty ops table never crashes */
    ranging_radio_core_request_off(&core);
    ranging_radio_core_step(&core);
    ranging_radio_core_request_on(&core);
    ranging_radio_core_step(&core);
    assert(core.max_start_tries == 1U);
}

int main(void)
{
    test_the_radio_goes_off_after_the_grace_time_and_comes_back_by_itself();
    test_leaving_before_the_radio_went_off_changes_nothing();
    test_a_radio_that_was_off_already_is_not_started_afterwards();
    test_a_refused_stop_does_not_hold_the_measurement_back();
    test_a_refused_start_is_retried_and_then_reported();
    test_a_new_measurement_during_the_return_waits_for_it();
    test_names_and_bad_arguments();
    return 0;
}
