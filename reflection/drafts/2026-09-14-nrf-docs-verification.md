# Candidate retrospective: nRF documentation verification

- Evidence: the focused documentation/task contract set passed 13/13. The
  full `python -m pytest -q` run reported 235 passed, 1 skipped and 2 failed.
- Root cause: existing `tools/run_recorder_sustained_hil.ps1` contains
  Windows-PowerShell-incompatible C-style unsigned literals (`0U` at lines
  282 and 319); the same file was clean and unmodified before this task.
- Recurrence count: 1 confirmed episode in this task; no generalized rule is
  proposed yet.
- Minimal proposed improvement: keep the existing PowerShell portability
  contract in the normal pre-merge gate and repair the unrelated runner in a
  separately approved tooling task, with a focused regression before changing
  any shared test command.
- Affected prior work: sustained-recorder HIL runner/contract work immediately
  preceding this documentation task. No change is proposed here because the
  user explicitly limited this task to nRF/architecture/test documentation.
