# Reflection draft: retain cleanup diagnostics and strict recursive scans

- Evidence: audit of main `a510291d8546` found `(void)storage_manager_remove_tree(path)` in the recorder cleanup path and an idempotent `while (readdir(...) != NULL)`/ignored `closedir()` in `storage_manager_remove_tree_locked()`. The first host compile of the new tree contract lacked the Windows dirent shim and had an `S_ISDIR`/`rmdir` stub mismatch; the corrected host run and full suite passed.
- Root cause: the primary recorder error path treated cleanup as best-effort without observable diagnostics, and the recursive deleter treated directory iteration as a boolean loop, so traversal failure was not an explicit result. The new contract test initially did not inherit the repository's Windows POSIX shim.
- Recurrence count: 1 confirmed episode in this task.
- Minimal proposed rule: secondary cleanup errors must be logged without replacing the primary error; every recursive filesystem enumeration must check both iteration error and directory close before destructive finalization. Host filesystem contracts should reuse the platform shim explicitly in their manifest.
- Affected area: `components/seismic_recorder/seismic_recorder_fs.c`, `components/storage_manager/storage_manager_tree.c`, and host storage/recorder contracts.
