# Status cache and runtime census rework

- Evidence: focused Python policy tests initially failed because the assertion still expected `RUNTIME_TASK name=` after the production log contract changed to `RUNTIME_TASK rank=... name=...`; after updating the contract test, 14/14 passed. The host suite then exposed a shared-header include coupling from `object_service.h`; localizing the event declaration restored 28/28 host tests. The graph-off target build also required correcting the SDKCONFIG path spelling and retrying a cached compiler path, after which the variant built successfully.
- Root cause: emitted-log/configuration contracts and shared host include surfaces were changed under separate assumptions; the first target retry also inherited a stale toolchain path from the failed configure.
- Recurrence count: 1 confirmed episode in this task; no generalized rule proposed.
- Proposed minimal rule: update source-contract tests with each emitted/config contract change, run the host manifest suite before target builds, and pass SDKCONFIG as an explicit absolute path in A/B builds.
- Affected prior tasks: none confirmed.
