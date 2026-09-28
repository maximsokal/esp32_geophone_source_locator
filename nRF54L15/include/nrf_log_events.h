#ifndef NRF_LOG_EVENTS_H
#define NRF_LOG_EVENTS_H

#include "nrf_log_protocol.h"

typedef enum {
#define X(id, name, subsystem, severity) NRF_LOG_EVENT_##name = id,
#include "nrf_log_events.def"
#undef X
} nrf_log_event_id_t;

#endif
