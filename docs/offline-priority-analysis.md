# Offline `web_status_cache` priority analysis

`tools/analyze_web_status_priority.py` analyzes saved UART logs without opening serial ports or changing task priorities.

It is intended for MAX-11: explaining the historical `web_status_cache` effective/current priority 23 observation without guessing or changing scheduler policy.

## Evidence sources

The analyzer accepts two source types:

1. Existing periodic runtime snapshots:
   `RUNTIME_TASK ... name=web_status_cache ... priority_current=... priority_base=... inherited=...`
2. Targeted storage-owner evidence (when present):
   `STORAGE_PRIORITY stage=pre-release owner=web_status_cache ...`

The targeted pre-release source is stronger for mutex-inheritance diagnosis because it is sampled while the task still owns the storage mutex, before the recursive mutex is released and an inherited boost can disappear.

## Basic use

```powershell
python .\tools\analyze_web_status_priority.py .\path\to\uart.log
```

Default expected base priority is `1`.

To require reproduction of an inherited boost:

```powershell
python .\tools\analyze_web_status_priority.py .\path\to\uart.log --require-inherited
```

To require targeted storage-owner evidence rather than relying only on top-N runtime snapshots:

```powershell
python .\tools\analyze_web_status_priority.py .\path\to\uart.log --require-storage-pre-release
```

For a longer capture, a minimum evidence count can also be required:

```powershell
python .\tools\analyze_web_status_priority.py .\path\to\uart.log --minimum-samples 3
```

## Result semantics

- `PASS` — captured samples are internally consistent and satisfy requested evidence requirements.
- `BLOCKED` — evidence is insufficient, for example no `web_status_cache` sample, no inherited sample when explicitly required, or no targeted storage marker when explicitly required.
- `FAIL` — evidence contradicts the scheduler contract, for example `priority_base != 1` or `inherited` does not agree with `priority_current > priority_base`.

A historical-style sample:

```text
priority_current=23 priority_base=1 inherited=1
```

is consistent with FreeRTOS mutex priority inheritance. A sample with `current=23 base=1 inherited=0` is inconsistent and fails the analyzer.

## Evidence boundary

This analyzer does not itself prove why a task was boosted. It validates the captured priority state. The strongest MAX-11 closure evidence is a targeted pre-release storage-owner sample showing the task still owns the storage path while `current > base` and `inherited=1`, together with no UI/input starvation.
