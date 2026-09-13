#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "object_service_internal.h"
#include "object_store.h"


unsigned fake_critical_depth;

static unsigned s_sequence;
static unsigned s_callback_sequence;
static unsigned s_observer_sequence;
static object_service_state_t *s_state;


esp_err_t object_store_get_active(object_summary_t *object)
{
    (void)object;
    return ESP_ERR_INVALID_STATE;
}


esp_err_t object_store_create(object_summary_t *object)
{
    memset(object, 0, sizeof(*object));
    strcpy(object->id, "OBJ_0001");
    return ESP_OK;
}


esp_err_t object_store_list(
    object_summary_t *objects,
    size_t capacity,
    size_t *count
)
{
    assert(objects != NULL);
    assert(capacity >= 1U);
    assert(count != NULL);
    memset(objects, 0, capacity * sizeof(*objects));
    *count = 0U;
    return ESP_OK;
}


void object_service_internal_process_mutation(
    const object_service_command_t *command
)
{
    (void)command;
}


static void on_callback(
    const object_service_event_t *event,
    void *context
)
{
    assert(event == &s_state->event);
    assert(context == s_state);
    assert(fake_critical_depth == 0U);
    assert(s_state->event_callback_in_flight == 1U);
    assert(s_state->event_observer_in_flight == 0U);

    s_callback_sequence = ++s_sequence;
}


static void on_observer(
    const object_service_event_t *event,
    void *context
)
{
    assert(event == &s_state->event);
    assert(context == &s_sequence);
    assert(fake_critical_depth == 0U);
    assert(s_state->event_callback_in_flight == 0U);
    assert(s_state->event_observer_in_flight == 1U);

    s_observer_sequence = ++s_sequence;
}


int main(void)
{
    s_state = object_service_internal_state();

    s_state->event_callback = on_callback;
    s_state->event_user_context = s_state;
    s_state->event_callback_in_flight = 0U;
    s_state->event_observer = on_observer;
    s_state->event_observer_context = &s_sequence;
    s_state->event_observer_in_flight = 0U;
    s_state->event.type = OBJECT_SERVICE_EVENT_LIST_DONE;
    s_state->event.result = ESP_OK;

    object_service_internal_notify(s_state);

    assert(s_sequence == 2U);
    assert(s_callback_sequence == 1U);
    assert(s_observer_sequence == 2U);
    assert(s_state->event_callback_in_flight == 0U);
    assert(s_state->event_observer_in_flight == 0U);
    assert(fake_critical_depth == 0U);

    /* A closed callback slot must not prevent the observer from running. */
    s_sequence = 0U;
    s_callback_sequence = 0U;
    s_observer_sequence = 0U;
    s_state->event_callback = NULL;
    s_state->event_user_context = NULL;

    object_service_internal_notify(s_state);

    assert(s_sequence == 1U);
    assert(s_callback_sequence == 0U);
    assert(s_observer_sequence == 1U);
    assert(s_state->event_callback_in_flight == 0U);
    assert(s_state->event_observer_in_flight == 0U);
    assert(fake_critical_depth == 0U);

    return 0;
}
