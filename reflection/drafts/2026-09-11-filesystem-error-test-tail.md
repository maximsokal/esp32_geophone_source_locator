# Filesystem error-path test tail

## Evidence

- Audit of main `4bb7867d96c1` identified ignored outer `closedir()` status and missing executable coverage for directory and known-present storage-read error paths.
- The first new host run failed to compile because the test used an object-like `stat` macro that renamed `struct stat` under MSVC.
- A function-like `stat(...)` seam fixed the compile issue; the final host run passed `30/30`, and the full suite passed.

## Root cause

The production change focused on the normal session-listing path and primary/backup routing. Cleanup/error handling at the outer directory boundary and a real storage text translation-unit test were not included in the initial verification surface. The test seam was also initially too broad for MSVC's tagged `struct stat` declaration.

## Recurrence count

1 confirmed episode in this task. This is evidence for a local test-design rule, not yet a generalized repository-wide rule.

## Proposed minimal rule

When a filesystem optimization adds a new branch or helper, add one executable test at the real translation-unit boundary for the fast path and fault injection for every newly surfaced open/read/close error. Use function-like libc call seams when a type shares the same identifier as the function.

## Affected prior work

- `4bb7867d96c1` recorder/storage/LVGL follow-up.
- Earlier `efb4d244` session census and known-primary read-path work.
