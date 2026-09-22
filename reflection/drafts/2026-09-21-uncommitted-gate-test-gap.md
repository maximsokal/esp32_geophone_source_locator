# Candidate retrospective: uncommitted recording-gate coverage

## Evidence

- The uncommitted recorder changes wired `recording_gate_evaluate()` into the real start path, but the existing integration test covered only the insufficient-remotes rejection and successful geometry path.
- The uncommitted Web status/UI changes added a new `recording_gate` JSON object and Record-button behavior without an executable C JSON test or a WebUI test for the admission predicate.
- The first new JSON host-test compile failed because the test fake declared function signatures without including the stub headers that own the snapshot and enum types.
- The first recorder integration run failed because the new not-level fixture still reported `assigned_count == 2`, so production correctly returned the insufficient-remotes error instead of `ESP_ERR_INVALID_STATE`; the fixture was corrected to `3 assigned / 2 accepted` and passed.

## Root cause

Coverage review had been performed at the pure-policy and source-contract layers, but the new integration boundaries (status serialization and UI admission) were not mapped back to executable tests. The hand-written host fake also duplicated type knowledge instead of including the test stubs.

## Recurrence count

1 confirmed episode in this repository; not enough evidence for a generalized process change.

## Proposed minimal rule/tool change

When an uncommitted change adds a cross-layer decision, require one executable test at each changed boundary: producer/start path, serialized status, and consumer predicate. For host fakes, include the same local stub headers as the production translation unit before declaring fake signatures. Add a small manifest helper that reports changed production files with no executable test entry.

## Affected prior tasks

- The recording-gate/MAX-165 uncommitted change set reviewed on 2026-09-21.
