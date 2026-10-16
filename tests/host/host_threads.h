#pragma once

#include <stdint.h>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#include <stdlib.h>

typedef SRWLOCK host_mutex_t;
typedef CONDITION_VARIABLE host_cond_t;
typedef HANDLE host_thread_t;
typedef void *(*host_thread_fn_t)(void *);

#define HOST_MUTEX_INITIALIZER SRWLOCK_INIT
#define HOST_COND_INITIALIZER CONDITION_VARIABLE_INIT

static inline int host_mutex_lock(host_mutex_t *mutex)
{
    AcquireSRWLockExclusive(mutex);
    return 0;
}

static inline int host_mutex_unlock(host_mutex_t *mutex)
{
    ReleaseSRWLockExclusive(mutex);
    return 0;
}

static inline int host_cond_wait_for(
    host_cond_t *condition,
    host_mutex_t *mutex,
    uint32_t timeout_ms
)
{
    if (SleepConditionVariableSRW(condition, mutex, timeout_ms, 0U)) {
        return 0;
    }
    return GetLastError() == ERROR_TIMEOUT ? 1 : -1;
}

static inline int host_cond_broadcast(host_cond_t *condition)
{
    WakeAllConditionVariable(condition);
    return 0;
}

typedef struct {
    host_thread_fn_t function;
    void *argument;
} host_thread_start_t;

static inline unsigned __stdcall host_thread_entry(void *opaque)
{
    host_thread_start_t start = *(host_thread_start_t *)opaque;
    free(opaque);
    (void)start.function(start.argument);
    return 0U;
}

static inline int host_thread_create(
    host_thread_t *thread,
    host_thread_fn_t function,
    void *argument
)
{
    host_thread_start_t *start = malloc(sizeof(*start));
    if (start == NULL) {
        return -1;
    }
    start->function = function;
    start->argument = argument;
    const uintptr_t handle = _beginthreadex(NULL, 0U, host_thread_entry, start, 0U, NULL);
    if (handle == 0U) {
        free(start);
        return -1;
    }
    *thread = (HANDLE)handle;
    return 0;
}

static inline int host_thread_join(host_thread_t thread)
{
    const DWORD result = WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return result == WAIT_OBJECT_0 ? 0 : -1;
}

static inline void host_thread_yield(void)
{
    SwitchToThread();
}

#else
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>

typedef pthread_mutex_t host_mutex_t;
typedef pthread_cond_t host_cond_t;
typedef pthread_t host_thread_t;
typedef void *(*host_thread_fn_t)(void *);

#define HOST_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
#define HOST_COND_INITIALIZER PTHREAD_COND_INITIALIZER

static inline int host_mutex_lock(host_mutex_t *mutex)
{
    return pthread_mutex_lock(mutex);
}

static inline int host_mutex_unlock(host_mutex_t *mutex)
{
    return pthread_mutex_unlock(mutex);
}

static inline int host_cond_wait_for(
    host_cond_t *condition,
    host_mutex_t *mutex,
    uint32_t timeout_ms
)
{
    struct timespec deadline;
    if (timespec_get(&deadline, TIME_UTC) != TIME_UTC) {
        return -1;
    }
    deadline.tv_sec += (time_t)(timeout_ms / 1000U);
    deadline.tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    const int result = pthread_cond_timedwait(condition, mutex, &deadline);
    return result == 0 ? 0 : (result == ETIMEDOUT ? 1 : -1);
}

static inline int host_cond_broadcast(host_cond_t *condition)
{
    return pthread_cond_broadcast(condition);
}

static inline int host_thread_create(
    host_thread_t *thread,
    host_thread_fn_t function,
    void *argument
)
{
    return pthread_create(thread, NULL, function, argument);
}

static inline int host_thread_join(host_thread_t thread)
{
    return pthread_join(thread, NULL);
}

static inline void host_thread_yield(void)
{
    (void)sched_yield();
}
#endif
