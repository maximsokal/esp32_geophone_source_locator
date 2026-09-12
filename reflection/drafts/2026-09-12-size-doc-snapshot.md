# Candidate retrospective: archive size documentation

- Evidence: the final map JSON reported `libespressif__button.a` as `7694 / 515` and `libsensor_bus.a` as `238 / 8`, while the baseline document retained earlier intermediate values `7681 / 515` and `192 / 8`.
- Root cause: the archive table was not re-read from the final map after the last target build.
- Recurrence count: one confirmed episode in this task; not enough evidence for a generalized rule.
- Minimal proposal: after the last build, compare every documented archive row against the final map JSON before the completion report.
- Affected artifact: only the repository-local baseline documentation was corrected; firmware source and image were unchanged.
