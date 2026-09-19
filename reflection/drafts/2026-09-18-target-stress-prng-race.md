# Candidate retrospective: target stress PRNG ownership

- Evidence: the sensor, network and scheduler FreeRTOS tasks all called
  `stress_random(phase)` against one shared `random_state` without a lock.
  That introduced a real cross-core write race and made jitter ordering depend
  on scheduler interleaving.
- Root cause: the stress workload was designed as concurrent code, but its
  helper state was modeled as if only one task owned it.
- Recurrence count: 1 confirmed episode in this task.
- Minimal proposed rule: every mutable generator/state used by a target task
  must have one owner or an explicit lock/atomic contract; source-contract
  tests should guard per-task ownership for deterministic stress inputs.
- Resolution: replaced the shared state with independent sensor/network/
  scheduler PRNG states seeded from the phase seed; target rebuild completed
  `9/9` and the stress contract test passed `2/2`.
