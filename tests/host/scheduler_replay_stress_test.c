#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>


#define SCHEDULE_EVENT_COUNT 64U
#define SCHEDULE_ISR_EVENT  255U


typedef struct
{
    uint32_t source_order;
    uint32_t event;
    uint32_t release_us;
    uint32_t jitter_us;

} schedule_event_t;


static uint32_t next_random(uint32_t *state)
{
    uint32_t value = *state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}


static void make_schedule(
    uint32_t seed,
    schedule_event_t events[SCHEDULE_EVENT_COUNT]
)
{
    uint32_t random_state = seed;
    for (uint32_t index = 0U; index < SCHEDULE_EVENT_COUNT; ++index) {
        events[index] = (schedule_event_t){
            .source_order = index,
            .event = index % 7U == 0U ? SCHEDULE_ISR_EVENT : index % 6U,
            .release_us = index * 1000U + next_random(&random_state) % 250U,
            .jitter_us = next_random(&random_state) % 500U,
        };
    }

    for (size_t index = SCHEDULE_EVENT_COUNT - 1U; index > 0U; --index) {
        const size_t other = next_random(&random_state) % (index + 1U);
        const schedule_event_t temporary = events[index];
        events[index] = events[other];
        events[other] = temporary;
    }
}


static uint32_t schedule_hash(
    const schedule_event_t events[SCHEDULE_EVENT_COUNT]
)
{
    uint32_t hash = 2166136261U;
    for (size_t index = 0U; index < SCHEDULE_EVENT_COUNT; ++index) {
        const uint32_t values[] = {
            events[index].source_order,
            events[index].event,
            events[index].release_us,
            events[index].jitter_us,
        };
        for (size_t value_index = 0U; value_index < 4U; ++value_index) {
            hash ^= values[value_index];
            hash *= 16777619U;
        }
    }
    return hash;
}


static size_t inversion_count(
    const schedule_event_t events[SCHEDULE_EVENT_COUNT]
)
{
    size_t inversions = 0U;
    for (size_t left = 0U; left < SCHEDULE_EVENT_COUNT; ++left) {
        for (size_t right = left + 1U; right < SCHEDULE_EVENT_COUNT; ++right) {
            if (events[left].source_order > events[right].source_order) {
                inversions++;
            }
        }
    }
    return inversions;
}


int main(void)
{
    static const uint32_t seed = 0x84AE1732U;
    schedule_event_t first[SCHEDULE_EVENT_COUNT];
    schedule_event_t replay[SCHEDULE_EVENT_COUNT];
    schedule_event_t other[SCHEDULE_EVENT_COUNT];

    make_schedule(seed, first);
    make_schedule(seed, replay);
    make_schedule(seed + 1U, other);

    const uint32_t first_hash = schedule_hash(first);
    const size_t inversions = inversion_count(first);
    size_t isr_events = 0U;
    uint32_t max_jitter_us = 0U;
    for (size_t index = 0U; index < SCHEDULE_EVENT_COUNT; ++index) {
        if (first[index].event == SCHEDULE_ISR_EVENT) {
            isr_events++;
        }
        if (first[index].jitter_us > max_jitter_us) {
            max_jitter_us = first[index].jitter_us;
        }
    }

    assert(memcmp(first, replay, sizeof(first)) == 0);
    assert(first_hash == schedule_hash(replay));
    assert(first_hash != schedule_hash(other));
    assert(inversions > 0U);
    assert(isr_events == 10U);
    assert(max_jitter_us < 500U);

    printf(
        "SCHEDULE_REPLAY seed=%u events=%u inversions=%u isr_events=%u "
        "max_jitter_us=%u trace_hash=%u verdict=PASS\n",
        (unsigned)seed,
        (unsigned)SCHEDULE_EVENT_COUNT,
        (unsigned)inversions,
        (unsigned)isr_events,
        (unsigned)max_jitter_us,
        (unsigned)first_hash
    );
    return 0;
}
