# Reflection candidate: DSP edge coverage found a cadence contract mismatch

## Evidence

- Commit `a17b251a` added bandpass, notch, FFT, and cadence-aware filtering with an existing DSP host test file.
- Initial Clang gcov coverage was `99.58%` lines and `98.81%` branches taken; the missing line was the non-positive NOTCH center guard.
- Added exact Nyquist validation, empty-block behavior, zero-FFT behavior, normalization-before-FFT ordering, NOTCH state reset, and stateless cadence behavior tests.
- The stateless cadence test failed against the committed implementation because it reported `cadence_reset_applied=true` whenever `cadence_contiguous=false`, including DC-only configurations. The implementation was corrected to report/reset only when BANDPASS or NOTCH is requested.
- Final DSP coverage was `100%` lines and `100%` branches; full host was `65/65` and Python was `547 passed, 1 skipped`.

## Root cause

The production code used the caller's cadence flag as a direct result flag instead of deriving the result from whether this configuration actually owned persistent filter state. Existing tests covered stateful BANDPASS reset but not the stateless contract boundary or NOTCH independently.

## Recurrence count

One confirmed cadence-result contract mismatch in this task. This is not yet a repeated pattern across separate tasks.

## Proposed minimal rule

Whenever a result field describes an internal action rather than an input flag, test at least one configuration where that internal action is absent. For every stateful sibling implementation, test reset behavior independently instead of assuming the first sibling test proves both.

## Affected prior tasks

The earlier MAX-133 DSP implementation and its initial host test addition; no other production component was found without a corresponding test in the reviewed latest-commit range.
