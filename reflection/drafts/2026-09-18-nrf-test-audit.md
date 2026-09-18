# nRF54L15 test-audit retrospective candidate

- Evidence: the canonical `nRF54L15/ncs.ps1 -Action build` initially stopped because `nrfutil` was installed under `C:\Users\Maxim\.nrfutil\bin` but absent from the process PATH. A process-local PATH correction allowed the pinned NCS v3.4.0 build to complete 610/610.
- Root cause: tool discovery depends on the caller's PATH even when the pinned SDK/tool is installed in the user's nrfutil directory.
- Recurrence count: 1 confirmed episode in this task; not enough evidence for a generalized rule change.
- Minimal proposal: have the wrapper or its verification guidance report the known per-user nrfutil location when PATH lookup fails, without changing build/flash behavior automatically.
- Affected work: XIAO nRF54L15 test audit and MAX-114 verification. No production code change was made for this blocker.
