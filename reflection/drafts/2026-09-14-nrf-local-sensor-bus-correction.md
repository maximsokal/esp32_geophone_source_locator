# Reflection draft: nRF local sensor-bus architecture correction

Date: 2026-09-14

## Evidence

- The earlier nRF documentation described the remote BME280 as separately
  powered and not mastered by the nRF54L15.
- The user supplied the accepted remote-node assembly: GY-BMI160 and BME280
  share the XIAO second I2C pair; both are powered from XIAO `3V3_OUT`; RJ45
  pin 2 +3.3 V is RESERVED/NC.
- The documentation was updated in `ARCHITECTURE.md`, the Phase-E I2C ADR,
  root/nRF READMEs, the nRF test task, validation/evidence docs and test-surface
  README. Search for the superseded separate-power wording returned no hits.

## Root cause

The previous documentation pass corrected the board variant and external IMU,
but did not propagate the later local-BME280 wiring decision through every
architecture and test-summary document. The stale statement survived because
the BME280 had been treated as an unrelated integration watchlist item.

## Recurrence count

Confirmed episodes: 1. This is evidence for a documentation consistency check,
not yet a generalized process rule.

## Minimal proposal

For the next hardware-topology change, search all architecture, ADR, test-plan,
validation, evidence and entry-point documents for the affected component and
its old power/bus ownership wording before finalizing the diff. Do not change
agent instructions or repository skills from this single episode.

## Verification

- `python -m pytest -q tests/python/test_nrf_platform_contract.py tests/python/test_architecture_refactor_contract.py tests/python/test_run_suite_contract.py tests/python/test_local_verify_runner.py tests/python/test_vscode_tasks.py` -> 22 passed; pytest emitted a non-failing Windows temp cleanup PermissionError at process exit.
- Relative Markdown-link check -> PASS for 10 changed documents.
- `git diff --check` -> PASS.
- Production-path diff check -> NONE.
