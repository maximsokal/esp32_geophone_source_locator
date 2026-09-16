#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "ui_event_policy.h"

int main(void)
{
    const int64_t queued = 1000000LL;

    assert(!ui_event_keypad_is_stale(queued, queued));
    assert(!ui_event_keypad_is_stale(
        queued,
        queued + UI_KEYPAD_EVENT_MAX_AGE_US
    ));
    assert(ui_event_keypad_is_stale(
        queued,
        queued + UI_KEYPAD_EVENT_MAX_AGE_US + 1LL
    ));

    /* A monotonic-clock discontinuity must never turn into a huge unsigned
     * age and discard a fresh event. */
    assert(!ui_event_keypad_is_stale(queued, queued - 1LL));

    assert(ui_event_setup_client_notice_allowed(
        true,
        true,
        false,
        false
    ));
    assert(!ui_event_setup_client_notice_allowed(
        false,
        true,
        false,
        false
    ));
    assert(!ui_event_setup_client_notice_allowed(
        true,
        false,
        false,
        false
    ));
    assert(!ui_event_setup_client_notice_allowed(
        true,
        true,
        true,
        false
    ));
    assert(!ui_event_setup_client_notice_allowed(
        true,
        true,
        false,
        true
    ));

    assert(ui_event_sta_notice_allowed(
        true,
        false,
        false,
        false
    ));
    assert(!ui_event_sta_notice_allowed(
        false,
        false,
        false,
        false
    ));
    assert(!ui_event_sta_notice_allowed(
        true,
        true,
        false,
        false
    ));
    assert(!ui_event_sta_notice_allowed(
        true,
        false,
        true,
        false
    ));
    assert(!ui_event_sta_notice_allowed(
        true,
        false,
        false,
        true
    ));

    puts("ui event freshness policy tests passed");
    return 0;
}
