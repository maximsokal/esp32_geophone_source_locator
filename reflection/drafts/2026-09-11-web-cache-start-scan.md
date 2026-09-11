# Candidate retrospective: Web cache startup and missing-session scan

- Evidence: `web_server_status_cache_start()` synchronously refreshed the object/session cache before registering HTTP/HTTPS, while session listing performed metadata existence checks that issued slow negative FAT `stat()` calls. The final local suite passed after replacing startup refresh with a queued cache-task request and using child-directory census before JSON reads.
- Root cause: a lifecycle cache operation was placed on the network-server critical path, and the session reader treated presumed metadata absence as a normal per-file probe instead of using the directory entries already available from the session census.
- Rework evidence: the first implementation used a stack `session_path` buffer and violated the repository storage-memory contract; a separate intermediate host run exposed an inverted fallback condition for metadata-less sessions. Both were corrected before the final host result.
- Recurrence count: one confirmed episode in this task. This is a candidate rule, not a generalized rule yet.
- Proposed minimal rule/tool change: for non-realtime storage scans, add the focused memory-policy and host-contract tests immediately after each path/allocation change, and require a test that proves the expensive metadata reader is not called for a directory with neither primary nor backup metadata.
- Affected prior work: Web status-cache lifecycle/arbitration, session listing performance, and the pending CPU0/TFT runtime diagnosis. No AGENTS.md, TOOLS.md, or skill was modified.
