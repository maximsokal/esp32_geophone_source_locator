# Phase C+D callback boundary correction

- Evidence: the initial adapter preserved the legacy startup callback path; the user explicitly required official Button callbacks to publish through `app_events`.
- Root cause: compatibility with `main/app_startup.c` was treated as the event-delivery boundary, even though the requested boundary was the app-events abstraction.
- Correction: keypad callbacks now post `APP_EVENT_KEYPAD` directly; power callbacks set the authoritative EventGroup bit first, then perform optional/app-event notification. The legacy startup API remains source-compatible and does not duplicate events.
- Recurrence count: 1 confirmed episode.
- Proposed minimal rule: when replacing an input worker with a vendor callback, verify the callback's final delivery abstraction explicitly before preserving legacy callback parameters.
- Affected task: Phase C+D keypad and power-button migration on 2026-09-12.
