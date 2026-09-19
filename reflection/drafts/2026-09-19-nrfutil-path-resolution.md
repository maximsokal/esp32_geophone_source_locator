# Reflection draft: nrfutil installed but invisible to VS Code task

## Evidence

- `nRF54L15/ncs.ps1 -Action flash` failed before build because `nrfutil` was not on the task PATH.
- The expected executable existed at `C:\Users\Maxim\.nrfutil\bin\nrfutil.exe` and reported version 8.2.1.
- After changing `Require-Command` to add the user-local bin directory to the current process PATH, the same flash task completed with exit code 0.
- OpenOCD detected the XIAO CMSIS-DAP and wrote 301260 bytes; the wrapper reported `nRF54L15 production flash: PASS`.

## Root cause

The wrapper detected the user-local installation but deliberately threw an instructional error instead of making that installation available to its own child processes. VS Code tasks do not necessarily inherit the updated interactive-shell PATH.

## Recurrence count

One confirmed episode in this repository. Do not generalize beyond Nordic wrapper scripts until another independent occurrence is confirmed.

## Proposed minimal rule/tool change

For user-scoped tools with a deterministic installation path, resolve and prepend the path for the current PowerShell process before invoking the tool; keep the environment change process-local and retain a clear failure when the executable is absent.

## Affected prior work

- Earlier XIAO flash attempts through `nRF54L15/ncs.ps1` were blocked before build/flash solely by PATH resolution, not by firmware or board connectivity.
