#include <assert.h>
#include <stdbool.h>

#include "ad7606_presence_policy.h"

static void observe_sequence(
    ad7606_presence_observer_t *observer,
    const int *levels,
    unsigned count
)
{
    for (unsigned i = 0; i < count; ++i) {
        ad7606_presence_observer_observe(observer, levels[i]);
    }
}

int main(void)
{
    ad7606_presence_observer_t observer;

    ad7606_presence_observer_reset(&observer, 0);
    for (unsigned i = 0; i < 20; ++i) ad7606_presence_observer_observe(&observer, 0);
    assert(!ad7606_presence_observer_ready(&observer));
    assert(observer.completed_pulses == 0U);

    ad7606_presence_observer_reset(&observer, 1);
    for (unsigned i = 0; i < 20; ++i) ad7606_presence_observer_observe(&observer, 1);
    assert(!ad7606_presence_observer_ready(&observer));
    assert(observer.completed_pulses == 0U);

    ad7606_presence_observer_reset(&observer, 0);
    const int incomplete[] = {1, 1, 1};
    observe_sequence(&observer, incomplete, sizeof(incomplete) / sizeof(incomplete[0]));
    assert(!ad7606_presence_observer_ready(&observer));
    assert(observer.completed_pulses == 0U);

    ad7606_presence_observer_reset(&observer, 0);
    const int two_pulses[] = {1, 0, 1, 0};
    observe_sequence(&observer, two_pulses, sizeof(two_pulses) / sizeof(two_pulses[0]));
    assert(!ad7606_presence_observer_ready(&observer));
    assert(observer.completed_pulses == 2U);

    ad7606_presence_observer_reset(&observer, 0);
    const int three_pulses[] = {1, 0, 1, 0, 1, 0};
    observe_sequence(&observer, three_pulses, sizeof(three_pulses) / sizeof(three_pulses[0]));
    assert(ad7606_presence_observer_ready(&observer));
    assert(observer.completed_pulses == AD7606_PRESENCE_REQUIRED_PULSES);

    /* Noise/repeated levels may not fabricate extra pulse completions. */
    ad7606_presence_observer_reset(&observer, 0);
    const int noisy[] = {0, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0};
    observe_sequence(&observer, noisy, sizeof(noisy) / sizeof(noisy[0]));
    assert(ad7606_presence_observer_ready(&observer));
    assert(observer.completed_pulses == 3U);

    return 0;
}
