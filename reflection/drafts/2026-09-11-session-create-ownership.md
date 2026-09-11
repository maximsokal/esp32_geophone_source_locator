# Reflection draft: session directory ownership and strict directory scans

- Evidence: audit of main `9d8306915a79` identified ignored `readdir()`/`closedir()` errors in `seismic_recorder_fs_next_session_id()` and ensure-directory behavior before `fopen(path, "wb")`. Host verification initially failed on a missing counter reset and an incorrectly duplicated test fake; the corrected run passed.
- Root cause: directory enumeration was treated as a boolean EOF loop, while session creation used an idempotent directory helper for a resource whose ownership must be exclusive. The test seam also initially reused an idempotent helper, so it did not model `EEXIST` faithfully.
- Recurrence count: 1 confirmed episode in this task.
- Minimal proposed rule: any filesystem census used to derive a new persistent name must distinguish EOF from `errno` failure and check directory close; creation of a derived resource must use an exclusive primitive and cleanup only resources created by that operation. Host fakes must preserve the production collision/error contract.
- Affected area: `components/seismic_recorder/seismic_recorder_fs.c`, `components/storage_manager/storage_manager_directory.c`, and their host contracts.
