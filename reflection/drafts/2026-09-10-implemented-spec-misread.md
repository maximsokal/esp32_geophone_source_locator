# Candidate retrospective: implementation specification was treated as review-only

- Evidence: the pasted task explicitly ordered stages B-F (tests, HTTPS fix, UI scheduler/optimization, recorder throughput), but the prior response only audited the committed implementation and reported remaining gaps. The user corrected this directly on 2026-09-10.
- Root cause: the follow-up wording about checking updated code was given more weight than the imperative requirements in the referenced specification.
- Recurrence count: 1 confirmed episode in this repository; this is not yet a generalized pattern.
- Minimal proposed change: when a referenced specification mixes audit and implementation, build a requirement checklist from its imperative deliverables and do not stop at audit if any requested code change remains unimplemented.
- Affected prior task: review of `427a8c4` on 2026-09-10.
- Validation for this correction: production changes plus host and full-suite verification are required before completion.
