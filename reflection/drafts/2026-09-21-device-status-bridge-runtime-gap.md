# Candidate retrospective: device-status bridge runtime coverage

- Evidence: commit `9bb01db3` changed the production `app_pod_status_bridge.c` runtime to fetch the active object/profile and publish per-pod local positions. The repository now has pure local-position policy tests, Python source contracts, a passing 21-suite nRF coverage manifest, and a passing ESP-IDF production build, but no executable host/integration test invokes the changed bridge with object-store and device-status fakes.
- Root cause: the test additions followed the extracted pure policy and source wiring, while treating the runtime bridge as covered by compile and text assertions. That leaves success/error state transitions, channel mapping, fetch cadence, task lifecycle, and coexistence with existing barometer publication unobserved.
- Recurrence count: 2 confirmed cross-layer episodes in the current work log (`MAX-165` recording gate and `MAX-118` device-status bridge) where pure/contract tests initially did not exercise the runtime boundary.
- Proposed minimal rule: every changed runtime bridge or admission boundary gets at least one executable host integration test with fakes for its direct dependencies; pure policy and source-contract tests remain supplemental.
- Affected prior tasks: `MAX-165` recording admission gate and `MAX-118` live per-pod local position.
