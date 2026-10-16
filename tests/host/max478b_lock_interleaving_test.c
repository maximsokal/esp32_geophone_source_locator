#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "host_threads.h"

#if defined(_WIN32)
typedef DWORD host_thread_id_t;

static host_thread_id_t host_current_thread_id(void)
{
    return GetCurrentThreadId();
}

static bool host_same_thread_id(host_thread_id_t left, host_thread_id_t right)
{
    return left == right;
}

static int host_mutex_try_lock(host_mutex_t *mutex)
{
    return TryAcquireSRWLockExclusive(mutex) ? 0 : 1;
}
#else
typedef host_thread_t host_thread_id_t;

static host_thread_id_t host_current_thread_id(void)
{
    return pthread_self();
}

static bool host_same_thread_id(host_thread_id_t left, host_thread_id_t right)
{
    return pthread_equal(left, right) != 0;
}

static int host_mutex_try_lock(host_mutex_t *mutex)
{
    return pthread_mutex_trylock(mutex);
}
#endif

typedef struct {
    host_mutex_t native;
    host_mutex_t owner_guard;
    host_thread_id_t owner;
    bool owned;
    unsigned takes;
    unsigned gives;
} tracked_mutex_state_t;

typedef struct {
    tracked_mutex_state_t storage;
    tracked_mutex_state_t spi;
    tracked_mutex_state_t lvgl;
    host_mutex_t step_mutex;
    host_cond_t step_changed;
    bool owner_ready;
    bool ui_ready;
    bool owner_waiting_for_lvgl;
    bool ui_waiting_for_storage;
    bool ui_released_lvgl;
    bool break_cycle;
    bool cycle_observed;
    bool release_before_storage;
} interleaving_state_t;

#define TRACKED_MUTEX_INITIALIZER \
    { .native = HOST_MUTEX_INITIALIZER, .owner_guard = HOST_MUTEX_INITIALIZER }

static interleaving_state_t s_state = {
    .storage = TRACKED_MUTEX_INITIALIZER,
    .spi = TRACKED_MUTEX_INITIALIZER,
    .lvgl = TRACKED_MUTEX_INITIALIZER,
    .step_mutex = HOST_MUTEX_INITIALIZER,
    .step_changed = HOST_COND_INITIALIZER,
};

static void tracked_mutex_reset(tracked_mutex_state_t *mutex)
{
    assert(host_mutex_lock(&mutex->owner_guard) == 0);
    assert(!mutex->owned);
    mutex->takes = 0U;
    mutex->gives = 0U;
    assert(host_mutex_unlock(&mutex->owner_guard) == 0);
}

static void tracked_mutex_take(tracked_mutex_state_t *mutex)
{
    assert(host_mutex_lock(&mutex->native) == 0);
    assert(host_mutex_lock(&mutex->owner_guard) == 0);
    assert(!mutex->owned);
    mutex->owned = true;
    mutex->owner = host_current_thread_id();
    ++mutex->takes;
    assert(host_mutex_unlock(&mutex->owner_guard) == 0);
}

static int tracked_mutex_try_take(tracked_mutex_state_t *mutex)
{
    const int result = host_mutex_try_lock(&mutex->native);
    if (result != 0) return result;

    assert(host_mutex_lock(&mutex->owner_guard) == 0);
    assert(!mutex->owned);
    mutex->owned = true;
    mutex->owner = host_current_thread_id();
    ++mutex->takes;
    assert(host_mutex_unlock(&mutex->owner_guard) == 0);
    return 0;
}

static void tracked_mutex_give(tracked_mutex_state_t *mutex)
{
    assert(host_mutex_lock(&mutex->owner_guard) == 0);
    assert(mutex->owned);
    assert(host_same_thread_id(mutex->owner, host_current_thread_id()));
    mutex->owned = false;
    ++mutex->gives;
    assert(host_mutex_unlock(&mutex->owner_guard) == 0);
    assert(host_mutex_unlock(&mutex->native) == 0);
}

static void tracked_mutex_assert_balanced(tracked_mutex_state_t *mutex)
{
    assert(host_mutex_lock(&mutex->owner_guard) == 0);
    assert(!mutex->owned);
    assert(mutex->takes == mutex->gives);
    assert(host_mutex_unlock(&mutex->owner_guard) == 0);
}

static void wait_for(bool *value, interleaving_state_t *state)
{
    assert(host_mutex_lock(&state->step_mutex) == 0);
    while (!*value) {
        assert(host_cond_wait_for(&state->step_changed, &state->step_mutex, 5000U) == 0);
    }
    assert(host_mutex_unlock(&state->step_mutex) == 0);
}

static void set_step(bool *value, interleaving_state_t *state)
{
    assert(host_mutex_lock(&state->step_mutex) == 0);
    *value = true;
    assert(host_cond_broadcast(&state->step_changed) == 0);
    assert(host_mutex_unlock(&state->step_mutex) == 0);
}

static void *storage_owner_task(void *argument)
{
    interleaving_state_t *state = argument;
    tracked_mutex_take(&state->storage);
    tracked_mutex_take(&state->spi);
    set_step(&state->owner_ready, state);
    wait_for(&state->ui_ready, state);

    assert(tracked_mutex_try_take(&state->lvgl) != 0);
    set_step(&state->owner_waiting_for_lvgl, state);

    wait_for(&state->ui_released_lvgl, state);
    tracked_mutex_take(&state->lvgl);
    tracked_mutex_give(&state->lvgl);
    tracked_mutex_give(&state->spi);
    tracked_mutex_give(&state->storage);
    return NULL;
}

static void *ui_storage_query_task(void *argument)
{
    interleaving_state_t *state = argument;
    tracked_mutex_take(&state->lvgl);
    wait_for(&state->owner_ready, state);
    set_step(&state->ui_ready, state);
    wait_for(&state->owner_waiting_for_lvgl, state);

    if (!state->release_before_storage) {
        assert(tracked_mutex_try_take(&state->storage) != 0);
        set_step(&state->ui_waiting_for_storage, state);
        wait_for(&state->break_cycle, state);
    }

    tracked_mutex_give(&state->lvgl);
    set_step(&state->ui_released_lvgl, state);

    tracked_mutex_take(&state->storage);
    tracked_mutex_take(&state->spi);
    tracked_mutex_take(&state->lvgl);
    tracked_mutex_give(&state->lvgl);
    tracked_mutex_give(&state->spi);
    tracked_mutex_give(&state->storage);
    return NULL;
}

static void interleaving_reset(interleaving_state_t *state, bool release_before_storage)
{
    tracked_mutex_reset(&state->storage);
    tracked_mutex_reset(&state->spi);
    tracked_mutex_reset(&state->lvgl);
    state->owner_ready = false;
    state->ui_ready = false;
    state->owner_waiting_for_lvgl = false;
    state->ui_waiting_for_storage = false;
    state->ui_released_lvgl = false;
    state->break_cycle = false;
    state->cycle_observed = false;
    state->release_before_storage = release_before_storage;
}

static bool run_interleaving(bool release_before_storage)
{
    interleaving_state_t *state = &s_state;
    interleaving_reset(state, release_before_storage);

    host_thread_t storage_owner;
    host_thread_t ui_task;
    assert(host_thread_create(&storage_owner, storage_owner_task, state) == 0);
    wait_for(&state->owner_ready, state);
    assert(host_thread_create(&ui_task, ui_storage_query_task, state) == 0);

    wait_for(&state->owner_waiting_for_lvgl, state);
    if (!release_before_storage) {
        wait_for(&state->ui_waiting_for_storage, state);
        assert(host_mutex_lock(&state->step_mutex) == 0);
        state->cycle_observed = true;
        state->break_cycle = true;
        assert(host_cond_broadcast(&state->step_changed) == 0);
        assert(host_mutex_unlock(&state->step_mutex) == 0);
    }

    assert(host_thread_join(storage_owner) == 0);
    assert(host_thread_join(ui_task) == 0);
    const bool cycle_observed = state->cycle_observed;
    tracked_mutex_assert_balanced(&state->storage);
    tracked_mutex_assert_balanced(&state->spi);
    tracked_mutex_assert_balanced(&state->lvgl);
    return cycle_observed;
}

static void test_original_order_reproduces_abba(void)
{
    assert(run_interleaving(false));
}

static void test_lvgl_release_before_storage_breaks_abba(void)
{
    assert(!run_interleaving(true));
}

int main(void)
{
    test_original_order_reproduces_abba();
    test_lvgl_release_before_storage_breaks_abba();
    puts("MAX478B: original ABBA reproduced; release-before-storage interleaving completed");
    return 0;
}
