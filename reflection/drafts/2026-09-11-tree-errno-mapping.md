# Reflection draft: directory-stream errno mapping

- Evidence: `storage_manager_tree_errno_to_error()` was shared by path-level `stat()` and already-open directory-stream failures. That allowed `readdir()` with `errno == ENOENT` to return `ESP_ERR_NOT_FOUND`, suppressing the recorder cleanup warning while the directory still existed.
- Root cause: one errno-to-domain-error helper was reused across operations with different error semantics.
- Recurrence count: 1 confirmed episode in this repository task.
- Minimal proposed rule: keep filesystem-path existence mapping local to the path check; use operation-specific mappings for directory-stream reads and closes, and add one regression for every errno class whose meaning differs by API.
- Affected verification: `storage_manager_tree_contract_test` now covers `readdir(ENOENT) -> ESP_FAIL`, `closedir` failure, operation guard failure, and successful recursive deletion.
