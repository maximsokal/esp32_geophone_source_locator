# Candidate retrospective: UI event callback boundary

- Evidence: the first APP_EVENT UI subscriber attempted to acquire the LVGL lock and retry with a delay from the user-loop callback. The corrected `ui_event_dispatcher.c` only copies into a bounded pending queue and wakes LVGL; `ui.c` drains the queue from the LVGL timer. The focused liveness contract now passes.
- Root cause: the event callback was treated as a UI execution context even though it runs on the dedicated application-event task and can overlap LVGL, SD, TLS and Wi-Fi lifecycle locks.
- Recurrence count: 1 confirmed episode.
- Proposed minimal rule: application-event handlers must be bounded enqueue/wake operations; any framework-owned object mutation runs from that framework's owner task or lock context.
- Affected task: Phase B event-loop migration and UI integration on 2026-09-12.
