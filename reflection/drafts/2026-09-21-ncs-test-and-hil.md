# Reflection candidate: nRF NCS test and Channel Sounding HIL boundary

## Evidence

- The first CS test-build invocation compiled the fallback path because the `NCS_NATIVE_CS` CMake variable did not propagate through NCS sysbuild.
- Passing explicit forward-slash `CONF_FILE` and `EXTRA_CONF_FILE` arguments to the application build produced the intended CS/RAS configuration and a 606-step target build with `BT_CS_DE` enabled.
- The first ESP32 accuracy-HIL build stopped at the repository's `storage_manager` compile-time FATFS contract. Adding the required `CONFIG_FATFS_LFN_HEAP`, `CONFIG_FATFS_MAX_LFN`, and `CONFIG_FATFS_FS_LOCK` defaults produced a 910/910 build.
- The full Python suite initially found two stale contract expectations and then one stale documentation phrase; after updating those tests, the final run was `547 passed, 1 skipped`.

## Root cause

The initial test-app design assumed ordinary CMake variables would reach an NCS sysbuild child and assumed the standalone HIL app inherited the production sdkconfig. Both assumptions were false in this checkout. Contract tests also lagged the deliberate removal of the old nRF `SKIPPED` stage.

## Recurrence count

One confirmed sysbuild configuration-propagation episode and one confirmed standalone-HIL configuration episode in this task. The stale-test episode is one maintenance occurrence; there is not yet evidence for a generalized recurring rule.

## Proposed minimal rule

For NCS sysbuild test applications, verify the generated child `.config` contains the intended feature symbols and pass child configuration explicitly when a profile changes. For standalone ESP-IDF HIL applications, build from an empty generated configuration and treat every production component compile-time requirement as an explicit app default. Update contract tests in the same patch as verification-plan stage changes.

## Affected prior tasks

The earlier nRF coverage audit correctly identified the CS runtime as build/contract-only but did not yet provide an executable target test-build or physical accuracy runner. The earlier geophone math edge-coverage work remains separate; this task adds the missing nRF bridge/CS and physical-measurement boundary without claiming RF or geometry accuracy from software builds.
