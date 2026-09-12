# Candidate retrospective: power queue-full regression stub

- Evidence: the first patch changed the return value of `app_events_post()` because the two stubs had identical assertion/count context; the adapter calls `app_events_post_best_effort()`, so the test still passed without exercising `ESP_ERR_TIMEOUT`.
- Root cause: a repeated code pattern made the patch context non-unique, and the test was not inspected after patching before the first full suite rerun.
- Recurrence count: one confirmed episode in this task; not enough evidence for a generalized repository rule.
- Minimal proposal: when adding a failure-injection stub with duplicate function bodies, inspect the complete target function pair and assert the injected return value is observed by the production call site before running aggregate validation.
- Affected verification: the first post-change full suite was superseded; the corrected host and full-suite results are the authoritative results.
