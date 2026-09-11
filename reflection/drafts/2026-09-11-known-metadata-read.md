# Candidate retrospective: known metadata path and error-preserving directory census

- Evidence: after the first session-directory optimization, an existing `session.json` still entered `storage_manager_recover_backup_locked()` and paid a primary `stat()` before `fopen()`. The metadata census also returned `false` for all child-directory failures. The final implementation routes confirmed primary files through a no-recovery read, keeps backup-only recovery, and returns filesystem errors from directory iteration.
- Root cause: the optimization conveyed only a boolean presence result, so it could not distinguish primary from backup and could not carry filesystem error state across the helper boundary.
- Rework evidence: the first full host run after the API change failed at link time because a contract test that embeds production `seismic_recorder_fs.c` lacked the new boundary stub. The stub was added and the subsequent host and full-suite runs passed.
- Recurrence count: one confirmed episode in this task. This is a candidate rule, not a generalized rule yet.
- Proposed minimal rule/tool change: when a performance optimization adds a new boundary hint, test all three states (absent, primary, backup) and compile every host target that embeds the affected production translation unit.
- Affected prior work: SD-backed status-cache refresh and storage read timing; no managed component or project instruction file was modified.
