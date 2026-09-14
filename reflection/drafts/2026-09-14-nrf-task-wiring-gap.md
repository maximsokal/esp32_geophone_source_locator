# Candidate retrospective: nRF test-task wiring gap

- Evidence: the first documentation pass created the nRF test plan and
  described the future runner, but `.vscode/tasks.json` had no nRF test task
  and `tools/run_suite.ps1` had no nRF step. The user identified the gap.
- Root cause: the work stopped at documenting the future test boundary and did
  not audit the executable task graph after editing the planning documents.
- Recurrence count: 1 confirmed episode. This is not enough evidence for a
  generalized process/skill change.
- Minimal corrective action: add and contract-test the path-safe
  `nRF54L15/pio.ps1 -Action test` launch, expose `Tests: nRF54L15`, and wire
  nRF build/test stages into both project-wide local runners. Keep an empty
  future test surface explicitly `SKIPPED`.
- Verification: wrapper test returned 0 with
  `nRF54L15 TEST_RESULT: SKIPPED`; focused task/runner contracts passed 12/12;
  no firmware or hardware upload was performed.
