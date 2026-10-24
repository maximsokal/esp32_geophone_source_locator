#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Single owner of the process-wide cJSON allocation hooks.
 *
 * cJSON_Parse() returns NULL both for malformed text and for an allocation
 * failure, and gives the caller no way to tell them apart. Code that makes a
 * destructive decision from "the file did not parse" (object recovery) must not
 * mistake a transient out-of-memory for corruption. Every allocation that goes
 * through the hooks installed here bumps a failure counter when it returns
 * NULL; a caller snapshots the counter around a parse and treats any increase
 * as "not judged".
 *
 * The counter is global, so a failure in an unrelated task during the window is
 * also reported. That errs on the safe side: the caller retries later instead
 * of acting.
 */

typedef void *(*cjson_alloc_guard_alloc_fn)(size_t size);

/* Idempotent. Installs the counting hooks (free_fn = free). On host builds the
 * allocator is only exercised through cjson_alloc_guard_alloc(). */
void cjson_alloc_guard_install(void);

/* Replaces the underlying allocator; NULL restores malloc. Thread-safe. */
void cjson_alloc_guard_set_allocator(cjson_alloc_guard_alloc_fn allocator);

/* The hook function itself: current allocator, counting a NULL result. */
void *cjson_alloc_guard_alloc(size_t size);

/* Records an allocation failure observed outside the hook (stubs, tests). */
void cjson_alloc_guard_note_failure(void);

/* Monotonic (wraps at 2^32); compare snapshots for inequality only. */
uint32_t cjson_alloc_guard_failures(void);
