#pragma once

/*
 * Keep the host navigation fixture ABI-identical to the production UI module.
 * ui_screen_registry.c and ui_navigation.c include their colocated
 * components/ui/ui_internal.h before any -I stub can override it. If this
 * host-side header duplicates ui_context_t, the test and production objects
 * can silently disagree about field offsets and corrupt the fixture state.
 *
 * Include the production header here and let its dependency includes resolve
 * through the host stub directories instead. This gives both sides one source
 * of truth for ui_context_t, enums, and driver function-pointer layout.
 */
#include "../../../../components/ui/ui_internal.h"
