# Candidate retrospective: transport, signal, and START freshness needed separate contracts

- Evidence: the completion audit found that ALL_00/ALL_FF/identical/repeated frames were being counted as transport failures, rail hits were feeding serial quality, and START admitted a merely running task without a recent successful sample.
- Root cause: one rolling serial-quality state represented both transport observations and analog signal observations, while recorder admission checked task configuration but not sample progress.
- Recurrence count: 1 confirmed episode in this repository; this is not yet a generalized pattern.
- Minimal proposed change: keep quiet/flat/repeated and rail/clipping counters diagnostic, derive transport quality only from transport signatures, expose signal quality separately, and gate START on latest-valid sample freshness derived from the configured sample period.
- Affected task: AD7606 serial-quality and recorder START follow-up on 2026-09-10.
- Validation for this correction: record the exact host, web, and ESP build results in the daily memory log before completion; no flash or monitor is authorized.
