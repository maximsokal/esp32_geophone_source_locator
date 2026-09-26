#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "web_server_lifecycle_policy.h"

typedef struct {
    bool mdns_ready;
    bool http_running;
    bool https_running;
    bool exited;
} fake_web_resources_t;

typedef struct {
    bool pending;
    web_lifecycle_state_t desired_state;
} fake_desired_state_mailbox_t;

typedef struct {
    bool held;
} fake_lifecycle_lock_t;

typedef struct {
    bool uncertain;
} fake_radio_state_t;

typedef struct {
    bool transition_active;
    bool deferred_state_valid;
    web_lifecycle_state_t deferred_state;
    web_lifecycle_state_t rollback_state;
} fake_radio_transition_t;


static void fake_desired_state_overwrite(
    fake_desired_state_mailbox_t *mailbox,
    web_lifecycle_state_t desired_state
)
{
    mailbox->desired_state = desired_state;
    mailbox->pending = true;
}


static void fake_lifecycle_lock_take(
    fake_lifecycle_lock_t *lock
)
{
    assert(!lock->held);
    lock->held = true;
}


static void fake_lifecycle_lock_give(
    fake_lifecycle_lock_t *lock
)
{
    assert(lock->held);
    lock->held = false;
}


static void fake_network_event_enqueue(
    fake_lifecycle_lock_t *lock,
    fake_desired_state_mailbox_t *mailbox,
    web_lifecycle_state_t event_state,
    const fake_radio_state_t *radio_state
)
{
    assert(radio_state != NULL);
    fake_lifecycle_lock_take(lock);
    const web_lifecycle_state_t effective_state =
        web_lifecycle_effective_network_state(
            event_state,
            radio_state->uncertain
        );
    fake_desired_state_overwrite(mailbox, effective_state);
    fake_lifecycle_lock_give(lock);
}


static void fake_authoritative_state_enqueue(
    fake_lifecycle_lock_t *lock,
    fake_desired_state_mailbox_t *mailbox,
    web_lifecycle_state_t desired_state
)
{
    fake_lifecycle_lock_take(lock);
    fake_desired_state_overwrite(mailbox, desired_state);
    fake_lifecycle_lock_give(lock);
}


static bool fake_desired_state_receive(
    fake_desired_state_mailbox_t *mailbox,
    web_lifecycle_state_t *desired_state
)
{
    if (!mailbox->pending) return false;

    *desired_state = mailbox->desired_state;
    mailbox->pending = false;
    return true;
}


static void fake_radio_transition_begin(
    fake_radio_transition_t *transition,
    fake_desired_state_mailbox_t *mailbox,
    web_lifecycle_state_t rollback_state
)
{
    assert(!transition->transition_active);
    transition->transition_active = true;
    transition->deferred_state_valid = false;
    transition->rollback_state = rollback_state;
    fake_desired_state_overwrite(
        mailbox,
        WEB_LIFECYCLE_STATE_QUIESCED
    );
}


static void fake_radio_transition_ack(
    fake_radio_transition_t *transition
)
{
    /* ACK means the worker is quiesced; it does not finish radio stop. */
    assert(transition->transition_active);
}


static void fake_radio_transition_request_state(
    fake_radio_transition_t *transition,
    fake_desired_state_mailbox_t *mailbox,
    web_lifecycle_state_t desired_state
)
{
    if (transition->transition_active) {
        transition->deferred_state = desired_state;
        transition->deferred_state_valid = true;
        return;
    }

    fake_desired_state_overwrite(mailbox, desired_state);
}


static void fake_radio_transition_reconcile_failure(
    fake_radio_transition_t *transition,
    fake_desired_state_mailbox_t *mailbox
)
{
    assert(transition->transition_active);
    transition->transition_active = false;

    const web_lifecycle_state_t state =
        transition->deferred_state_valid
            ? transition->deferred_state
            : transition->rollback_state;
    fake_desired_state_overwrite(mailbox, state);
    transition->deferred_state_valid = false;
}


static void fake_radio_transition_complete(
    fake_radio_transition_t *transition,
    fake_desired_state_mailbox_t *mailbox,
    bool radio_stop_succeeded
)
{
    assert(transition->transition_active);

    if (!radio_stop_succeeded) {
        fake_radio_transition_reconcile_failure(transition, mailbox);
        return;
    }

    transition->transition_active = false;
    if (!transition->deferred_state_valid) {
        return;
    }

    fake_desired_state_overwrite(mailbox, transition->deferred_state);
    transition->deferred_state_valid = false;
}


static void apply_action(
    web_lifecycle_action_t action,
    fake_web_resources_t *resources
)
{
    switch (action) {
    case WEB_LIFECYCLE_ACTION_MDNS_START:
        resources->mdns_ready = true;
        break;
    case WEB_LIFECYCLE_ACTION_MDNS_STOP:
        resources->mdns_ready = false;
        break;
    case WEB_LIFECYCLE_ACTION_HTTP_START:
        resources->http_running = true;
        break;
    case WEB_LIFECYCLE_ACTION_HTTP_STOP:
        resources->http_running = false;
        break;
    case WEB_LIFECYCLE_ACTION_HTTPS_START:
        resources->https_running = true;
        break;
    case WEB_LIFECYCLE_ACTION_HTTPS_STOP:
        resources->https_running = false;
        break;
    case WEB_LIFECYCLE_ACTION_EXIT:
        resources->exited = true;
        break;
    default:
        assert(false);
        break;
    }
}

static void assert_plan(
    web_lifecycle_state_t state,
    const web_lifecycle_action_t *expected,
    size_t expected_count
)
{
    web_lifecycle_action_t actual[WEB_LIFECYCLE_ACTIONS_MAX] = { 0 };
    assert(web_lifecycle_state_actions(
        state,
        actual,
        sizeof(actual) / sizeof(actual[0])
    ) == expected_count);

    for (size_t i = 0U; i < expected_count; ++i) {
        assert(actual[i] == expected[i]);
    }
}

static void apply_plan(
    web_lifecycle_state_t state,
    fake_web_resources_t *resources
)
{
    web_lifecycle_action_t actions[WEB_LIFECYCLE_ACTIONS_MAX] = { 0 };
    const size_t action_count = web_lifecycle_state_actions(
        state,
        actions,
        sizeof(actions) / sizeof(actions[0])
    );
    assert(action_count > 0U);

    for (size_t i = 0U; i < action_count; ++i) {
        apply_action(actions[i], resources);
    }
}


static void apply_mode_transition_final_state(
    fake_web_resources_t *resources,
    bool running,
    bool setup_mode
)
{
    apply_plan(
        running && setup_mode
            ? WEB_LIFECYCLE_STATE_SETUP_AP
            : WEB_LIFECYCLE_STATE_STA_DISCONNECTED,
        resources
    );
}

static void apply_plan_with_injected_state(
    web_lifecycle_state_t state,
    fake_web_resources_t *resources,
    fake_desired_state_mailbox_t *mailbox,
    web_lifecycle_action_t inject_after,
    web_lifecycle_state_t injected_state
)
{
    web_lifecycle_action_t actions[WEB_LIFECYCLE_ACTIONS_MAX] = { 0 };
    const size_t action_count = web_lifecycle_state_actions(
        state,
        actions,
        sizeof(actions) / sizeof(actions[0])
    );
    assert(action_count > 0U);

    bool injected = false;
    for (size_t i = 0U; i < action_count; ++i) {
        apply_action(actions[i], resources);
        if (!injected && actions[i] == inject_after) {
            fake_desired_state_overwrite(mailbox, injected_state);
            injected = true;
        }
    }
}

int main(void)
{
    assert(web_lifecycle_command_ingress_allowed(false));
    assert(!web_lifecycle_command_ingress_allowed(true));
    assert(!web_lifecycle_small_body_timeout_retry_allowed());

    const web_lifecycle_action_t connected[] = {
        WEB_LIFECYCLE_ACTION_HTTP_STOP,
        WEB_LIFECYCLE_ACTION_HTTPS_START,
        WEB_LIFECYCLE_ACTION_MDNS_START,
    };
    const web_lifecycle_action_t setup_ap[] = {
        WEB_LIFECYCLE_ACTION_MDNS_STOP,
        WEB_LIFECYCLE_ACTION_HTTPS_STOP,
        WEB_LIFECYCLE_ACTION_HTTP_START,
    };
    const web_lifecycle_action_t disconnected[] = {
        WEB_LIFECYCLE_ACTION_MDNS_STOP,
        WEB_LIFECYCLE_ACTION_HTTPS_STOP,
        WEB_LIFECYCLE_ACTION_HTTP_STOP,
    };
    const web_lifecycle_action_t quiesced[] = {
        WEB_LIFECYCLE_ACTION_MDNS_STOP,
        WEB_LIFECYCLE_ACTION_HTTPS_STOP,
    };
    const web_lifecycle_action_t stopped[] = {
        WEB_LIFECYCLE_ACTION_MDNS_STOP,
        WEB_LIFECYCLE_ACTION_HTTPS_STOP,
        WEB_LIFECYCLE_ACTION_HTTP_STOP,
        WEB_LIFECYCLE_ACTION_EXIT,
    };

    assert_plan(WEB_LIFECYCLE_STATE_STA_CONNECTED, connected, 3U);
    assert_plan(WEB_LIFECYCLE_STATE_SETUP_AP, setup_ap, 3U);
    assert_plan(WEB_LIFECYCLE_STATE_STA_DISCONNECTED, disconnected, 3U);
    assert_plan(WEB_LIFECYCLE_STATE_QUIESCED, quiesced, 2U);
    assert_plan(WEB_LIFECYCLE_STATE_STOPPED, stopped, 4U);

    assert(web_lifecycle_state_actions(
        (web_lifecycle_state_t)99,
        NULL,
        0U
    ) == 0U);
    assert(web_lifecycle_state_actions(
        WEB_LIFECYCLE_STATE_STOPPED,
        NULL,
        0U
    ) == 4U);
    web_lifecycle_action_t truncated[2] = { 0 };
    assert(web_lifecycle_state_actions(
        WEB_LIFECYCLE_STATE_STOPPED,
        truncated,
        sizeof(truncated) / sizeof(truncated[0])
    ) == 4U);
    assert(truncated[0] == WEB_LIFECYCLE_ACTION_MDNS_STOP);
    assert(truncated[1] == WEB_LIFECYCLE_ACTION_HTTPS_STOP);
    assert(web_lifecycle_effective_network_state(
        WEB_LIFECYCLE_STATE_SETUP_AP,
        false
    ) == WEB_LIFECYCLE_STATE_SETUP_AP);
    assert(web_lifecycle_effective_network_state(
        WEB_LIFECYCLE_STATE_SETUP_AP,
        true
    ) == WEB_LIFECYCLE_STATE_STA_DISCONNECTED);

    fake_web_resources_t resources = { 0 };
    apply_plan(WEB_LIFECYCLE_STATE_STA_CONNECTED, &resources);
    assert(resources.mdns_ready);
    assert(!resources.http_running);
    assert(resources.https_running);

    /* AP_START overwrites the mailbox while the worker is still applying the
     * connected plan. The worker must finish that plan, then reconcile the
     * latest state and leave only the setup HTTP server running. */
    fake_desired_state_mailbox_t mailbox = { 0 };
    fake_desired_state_overwrite(
        &mailbox,
        WEB_LIFECYCLE_STATE_STA_CONNECTED
    );
    web_lifecycle_state_t queued_state;
    assert(fake_desired_state_receive(&mailbox, &queued_state));
    apply_plan_with_injected_state(
        queued_state,
        &resources,
        &mailbox,
        WEB_LIFECYCLE_ACTION_MDNS_START,
        WEB_LIFECYCLE_STATE_SETUP_AP
    );
    assert(mailbox.pending);
    assert(fake_desired_state_receive(&mailbox, &queued_state));
    assert(queued_state == WEB_LIFECYCLE_STATE_SETUP_AP);
    apply_plan(queued_state, &resources);
    assert(!resources.mdns_ready);
    assert(resources.http_running);
    assert(!resources.https_running);

    /* A late AP_START can begin before target cleanup fails, then reach the
     * enqueue boundary afterward. The uncertainty decision must be made while
     * holding the same lifecycle lock as the authoritative queue overwrite;
     * otherwise this event can resurrect HTTP after DISCONNECTED wins. */
    resources = (fake_web_resources_t){ 0 };
    apply_plan(WEB_LIFECYCLE_STATE_STA_CONNECTED, &resources);
    mailbox = (fake_desired_state_mailbox_t){ 0 };
    fake_lifecycle_lock_t lifecycle_lock = { 0 };
    const web_lifecycle_state_t late_event_state =
        WEB_LIFECYCLE_STATE_SETUP_AP;
    fake_radio_state_t radio_state = { false };
    /* The event derived AP before the target cleanup failure became visible. */
    assert(web_lifecycle_effective_network_state(
        late_event_state,
        radio_state.uncertain
    ) == WEB_LIFECYCLE_STATE_SETUP_AP);
    /* Cleanup failure publishes uncertainty before authoritative DISCONNECTED. */
    radio_state.uncertain = true;
    fake_authoritative_state_enqueue(
        &lifecycle_lock,
        &mailbox,
        WEB_LIFECYCLE_STATE_STA_DISCONNECTED
    );
    fake_network_event_enqueue(
        &lifecycle_lock,
        &mailbox,
        late_event_state,
        &radio_state
    );
    assert(fake_desired_state_receive(&mailbox, &queued_state));
    assert(queued_state == WEB_LIFECYCLE_STATE_STA_DISCONNECTED);
    apply_plan(queued_state, &resources);
    assert(!resources.mdns_ready);
    assert(!resources.http_running);
    assert(!resources.https_running);
    assert(!lifecycle_lock.held);

    /* The prepare ACK must not open the lifecycle gate before the radio stop
     * itself completes. Network events are deferred, latest-state wins, and
     * only then does the worker reconcile the state. */
    fake_radio_transition_t transition = { 0 };
    mailbox = (fake_desired_state_mailbox_t){ 0 };
    fake_radio_transition_begin(
        &transition,
        &mailbox,
        WEB_LIFECYCLE_STATE_STA_CONNECTED
    );
    assert(fake_desired_state_receive(&mailbox, &queued_state));
    assert(queued_state == WEB_LIFECYCLE_STATE_QUIESCED);
    apply_plan(queued_state, &resources);
    fake_radio_transition_ack(&transition);
    assert(transition.transition_active);
    assert(!resources.mdns_ready);
    assert(!resources.https_running);

    fake_radio_transition_request_state(
        &transition,
        &mailbox,
        WEB_LIFECYCLE_STATE_STA_CONNECTED
    );
    fake_radio_transition_request_state(
        &transition,
        &mailbox,
        WEB_LIFECYCLE_STATE_SETUP_AP
    );
    assert(!mailbox.pending);
    assert(transition.deferred_state_valid);

    fake_radio_transition_complete(&transition, &mailbox, true);
    assert(!transition.transition_active);
    assert(fake_desired_state_receive(&mailbox, &queued_state));
    assert(queued_state == WEB_LIFECYCLE_STATE_SETUP_AP);
    apply_plan(queued_state, &resources);
    assert(!resources.mdns_ready);
    assert(resources.http_running);
    assert(!resources.https_running);

    /* STA -> AP target start failure reports the disconnected fallback to the
     * web worker after the radio stop itself has already succeeded. */
    resources = (fake_web_resources_t){ 0 };
    apply_plan(WEB_LIFECYCLE_STATE_STA_CONNECTED, &resources);
    mailbox = (fake_desired_state_mailbox_t){ 0 };
    transition = (fake_radio_transition_t){ 0 };
    fake_radio_transition_begin(
        &transition,
        &mailbox,
        WEB_LIFECYCLE_STATE_STA_CONNECTED
    );
    assert(fake_desired_state_receive(&mailbox, &queued_state));
    apply_plan(queued_state, &resources);
    fake_radio_transition_ack(&transition);
    fake_radio_transition_complete(&transition, &mailbox, true);
    assert(!mailbox.pending);
    apply_mode_transition_final_state(&resources, false, false);
    assert(!resources.mdns_ready);
    assert(!resources.http_running);
    assert(!resources.https_running);

    /* AP -> STA target start failure must also stop the setup HTTP server; a
     * quiesced AP plan intentionally left it alive for the transition caller. */
    resources = (fake_web_resources_t){ 0 };
    apply_plan(WEB_LIFECYCLE_STATE_SETUP_AP, &resources);
    mailbox = (fake_desired_state_mailbox_t){ 0 };
    transition = (fake_radio_transition_t){ 0 };
    fake_radio_transition_begin(
        &transition,
        &mailbox,
        WEB_LIFECYCLE_STATE_SETUP_AP
    );
    assert(fake_desired_state_receive(&mailbox, &queued_state));
    apply_plan(queued_state, &resources);
    assert(resources.http_running);
    fake_radio_transition_ack(&transition);
    fake_radio_transition_complete(&transition, &mailbox, true);
    assert(!mailbox.pending);
    apply_mode_transition_final_state(&resources, false, false);
    assert(!resources.mdns_ready);
    assert(!resources.http_running);
    assert(!resources.https_running);

    /* A failed radio stop must restore the pre-transition stable state when
     * no newer network state arrived during the barrier. */
    resources = (fake_web_resources_t){ 0 };
    apply_plan(WEB_LIFECYCLE_STATE_STA_CONNECTED, &resources);
    mailbox = (fake_desired_state_mailbox_t){ 0 };
    transition = (fake_radio_transition_t){ 0 };
    fake_radio_transition_begin(
        &transition,
        &mailbox,
        WEB_LIFECYCLE_STATE_STA_CONNECTED
    );
    assert(fake_desired_state_receive(&mailbox, &queued_state));
    apply_plan(queued_state, &resources);
    fake_radio_transition_ack(&transition);
    fake_radio_transition_complete(&transition, &mailbox, false);
    assert(fake_desired_state_receive(&mailbox, &queued_state));
    assert(queued_state == WEB_LIFECYCLE_STATE_STA_CONNECTED);
    apply_plan(queued_state, &resources);
    assert(resources.mdns_ready);
    assert(!resources.http_running);
    assert(resources.https_running);

    /* A failed prepare after partial teardown follows the same rollback
     * policy even though esp_wifi_stop() is never reached. */
    mailbox = (fake_desired_state_mailbox_t){ 0 };
    transition = (fake_radio_transition_t){ 0 };
    fake_radio_transition_begin(
        &transition,
        &mailbox,
        WEB_LIFECYCLE_STATE_STA_CONNECTED
    );
    assert(fake_desired_state_receive(&mailbox, &queued_state));
    apply_plan(queued_state, &resources);
    fake_radio_transition_reconcile_failure(&transition, &mailbox);
    assert(fake_desired_state_receive(&mailbox, &queued_state));
    assert(queued_state == WEB_LIFECYCLE_STATE_STA_CONNECTED);
    apply_plan(queued_state, &resources);
    assert(resources.mdns_ready);
    assert(!resources.http_running);
    assert(resources.https_running);

    /* A newer deferred state remains authoritative even when the radio stop
     * fails; rollback is only the fallback when no such state exists. */
    mailbox = (fake_desired_state_mailbox_t){ 0 };
    transition = (fake_radio_transition_t){ 0 };
    fake_radio_transition_begin(
        &transition,
        &mailbox,
        WEB_LIFECYCLE_STATE_STA_CONNECTED
    );
    assert(fake_desired_state_receive(&mailbox, &queued_state));
    apply_plan(queued_state, &resources);
    fake_radio_transition_ack(&transition);
    fake_radio_transition_request_state(
        &transition,
        &mailbox,
        WEB_LIFECYCLE_STATE_SETUP_AP
    );
    fake_radio_transition_complete(&transition, &mailbox, false);
    assert(fake_desired_state_receive(&mailbox, &queued_state));
    assert(queued_state == WEB_LIFECYCLE_STATE_SETUP_AP);
    apply_plan(queued_state, &resources);
    assert(!resources.mdns_ready);
    assert(resources.http_running);
    assert(!resources.https_running);

    apply_plan(WEB_LIFECYCLE_STATE_STA_DISCONNECTED, &resources);
    assert(!resources.mdns_ready);
    assert(!resources.http_running);
    assert(!resources.https_running);

    apply_plan(WEB_LIFECYCLE_STATE_STA_CONNECTED, &resources);
    assert(resources.mdns_ready);
    assert(!resources.http_running);
    assert(resources.https_running);

    apply_plan(WEB_LIFECYCLE_STATE_QUIESCED, &resources);
    assert(!resources.mdns_ready);
    assert(!resources.https_running);

    apply_plan(WEB_LIFECYCLE_STATE_STOPPED, &resources);
    assert(!resources.mdns_ready);
    assert(!resources.http_running);
    assert(!resources.https_running);
    assert(resources.exited);

    puts("web lifecycle policy tests passed");
    return 0;
}
