# Performance stress profile correction

- Evidence: the first `performance_stress` run classified `steady_5000hz` as saturated because the scenario assigned 100 us DSP plus 300 us background work to a 200 us sample period. The model itself exposed `cpu_deadline_misses=2`; reducing the test-only background budget to 50 us made the intended healthy scenario pass. The deliberately slow-SD scenario still reports cache overflow and SPI2 queue overflow as `SATURATED`.
- Root cause: the initial workload profile mixed a periodic background budget into every sample-rate deadline without checking that the budget fit the selected rate.
- Recurrence count: 1 confirmed episode in this task; not enough evidence for a generalized project rule.
- Minimal proposed rule: every synthetic stress scenario should document `period_us`, per-frame work, periodic burst work, and expected classification; validate the arithmetic before treating a scenario as a firmware regression.
- Affected prior tasks: none identified.
