# Candidate retrospective: local firmware verification, 2026-09-08

Status: proposal only; no AGENTS.md, TOOLS.md or skill change applied.

Evidence:
- The initial production build used the correct cached Python 3.11.2/IDF 5.5.4, but default Ninja parallelism ended with a GCC internal compiler error in third-party esp_lcd_panel_rgb.c:700 (build/verification-baseline.log, exit2).
- A retry with the same toolchain/cache and `ninja -C build -j 2` completed (build/verification-baseline-retry.log, exit0). ESP_ROM_ELF_DIR was also set to eliminate a separate gdbinit environment warning.
- The default Windows Node locale is uk-UA. XR baseline had 581 passing and one failing test because meshQualityUi expected an English thousands separator. LANG/LC_ALL did not change the Windows ICU default.

Root cause:
- Compiler crash cause remains unproven. Lower parallelism correlated with successful retry; do not label resource pressure as established.
- XR assertion mixed a locale-neutral behavior contract with a hard-coded English rendering expectation.

Recurrence count: one confirmed task episode for each issue. This is not evidence of a project-wide pattern.

Proposed minimal action for this task: document the tested build environment and bounded retry command; make only the locale-dependent expected test string portable. Keep the original failures in the report. Do not globally change build policy or locale on this evidence.

Affected prior tasks: none established; historical Python-environment notes in the attachment are context, not a verified recurrence of this compiler/locale issue.

Additional verification episodes:
- A parallel invocation of the manifest runner targeted the same per-test output directories and produced a linker permission failure for `web_webui_flash`; a sequential rerun completed 17/17. The runner is intentionally documented and used sequentially; no concurrent-run guarantee is proposed from this one episode.
- Strict LLVM-MinGW host compilation exposed three portability issues (MinGW `dirent` did not expose the same `d_type` macros, a missing `stddef.h` for `NULL`, and log-only symbols becoming unused). Conditional guards, the include, and explicit void uses fixed them; subsequent normal, sanitizer, and coverage runs were green.
- The first Unity target build command pointed at a nonexistent `python_env/Scripts/idf.py`; the corrected ESP-IDF `tools/idf.py` invocation passed. This was a command-path error, not a project/toolchain defect.

Recurrence count for each additional issue: one confirmed episode. These observations do not justify generalized concurrency or environment rules beyond the local commands documented for this repository.

Follow-up episodes:
- The first serial preflight treated stale PnP records as usable ports. The subsequent esptool/monitor attempts failed with `FileNotFoundError`; `Get-PnpDevice` showed `Unknown`/`CM_PROB_PHANTOM`, while `SerialPort.GetPortNames()` was empty. The preflight now requires live serial enumeration and retains PnP only in the diagnostic message.
- The first HIL helper launch exposed two PowerShell details: script-array port arguments were not accepted as separate command-line values, and `Start-Process -ArgumentList` split the repository path at spaces. Tasks now pass the port list as `COM4,COM6`, and the monitor script quotes the helper path before starting it.

Recurrence count for each follow-up issue: one confirmed episode. Proposed minimal rule: hardware preflight must validate a live openable-port enumeration, and PowerShell child-process arguments containing workspace paths must be explicitly quoted. No global skill or repository instruction change is proposed from one episode.

Suite-report follow-up:
- The pasted VS Code task logs ended after the last dependency with only `Terminal will be reused by tasks`, so they did not identify which earlier test step failed or provide a reliable aggregate exit signal.
- A sequential `tools/run_suite.ps1` wrapper now records each local step, prints a PASS/FAIL/BLOCKED table, emits `SUITE_RESULT`, and keeps hardware HIL visibly SKIPPED in the no-flash aggregate. Web and full local suites both produced `SUITE_RESULT: PASS` with exit 0.

Recurrence count: one confirmed reporting episode. Proposed minimal action: keep aggregate local tasks behind a deterministic summary runner and retain separate hardware HIL evidence. No generalized CI/CD or task policy change is proposed from this single episode.

- Pipeline boundary follow-up: added a real production-helper host contract for recorder binary and web seismic JSON parity; MSVC 20/20 passed. ASan via MSVC was attempted but blocked by cl.exe C5072 under /WX; no second recurrence established for a generalized rule.

Final verification correction, 2026-09-08:
- Evidence: the explicit LLVM-MinGW Clang executable completed all 20 host suites with address/undefined sanitizers and all 20 with coverage; python -m pytest -q completed 6 passed/1 skipped; the HIL replay fixture passed, the read-timeout fixture failed, and live preflight returned BLOCKED before monitor/flash because COM4/COM6 were absent.
- Root cause/correction: the draft's earlier MSVC ASan limitation was a toolchain-specific observation, not the final sanitizer state. The runner's first live preflight output was overly verbose; parent normalization now reports the compact missing-port diagnostic while preserving the nonzero result.
- Recurrence count: one confirmed episode for the stale sanitizer statement and one for the verbose preflight output. These are local evidence corrections, not a project-wide pattern.
- Proposed minimal action: in future reports, identify the exact compiler invocation behind sanitizer claims and retain a short, stable preflight summary with the actionable external-state condition. No generalized skill, AGENTS.md, TOOLS.md, CI/CD, or task-policy change is proposed.
- Affected prior tasks: this firmware verification task only.

Execution note:
- A concurrent orchestration of host tests and two replay processes surfaced one inconsistent reported exit code for the otherwise PASS replay. A sequential direct process invocation exited 0, while the FAIL fixture exited 1 and live no-port exited 1 as intended.
- Recurrence count: one new orchestration/reporting anomaly. Root cause is not established; no runner change is justified. Use sequential commands for final exit-code evidence, consistent with the existing runner note.

Live-boundary follow-up, 2026-09-08:
- Evidence: no shared production heartbeat/latency counter exists for UI, keypad and power tasks; physical TFT/input behavior therefore remains operator-observed. The runner now records those observations only through explicit live confirmation switches and keeps replay confirmations BLOCKED. It also validates completed SD session metadata after recording stop.
- Parent review found one partial-pass edge case in the new confirmation switches and fixed it by requiring -RequireFullPipeline. Recurrence count: one confirmed episode; no generalized project rule is proposed.
- Proposed minimal action: retain explicit operator attestations and physical limitations in the HIL report; add automated task telemetry only if a measured runtime requirement later justifies a production API change.
Final current verification addendum, 2026-09-08:
- Sequential evidence: host MSVC 20/20, Clang ASan/UBSan 20/20, Clang coverage 20/20, pytest 7 passed/1 skipped, and `run_suite.ps1 -Suite All` exited 0 with `SUITE_RESULT: PASS` plus intentional hardware `HIL_RESULT: SKIPPED`.
- The valid and failing HIL replay fixtures still produce PASS/FAIL with child exits 0/1; live full-pipeline preflight remains BLOCKED with child exit 1 because COM4/COM6 are absent. No new rule is proposed; physical validation awaits external hardware state.

Runtime-observability follow-up, 2026-09-08:
- Evidence: the explicit user requirement included proving that FreeRTOS services remain responsive during the live Measurements pipeline. The existing production API had no shared heartbeat/progress signal, so a bounded runtime_observability component was added and wired to the existing acquisition, recorder, UI, keypad and power-button loops. /api/status.runtime.tasks is checked across two snapshots; recording additionally checks acquisition/recorder progress. Host, sanitizer, coverage, Python and ESP-IDF builds passed after the change.
- Root cause: static host contracts and compile success can show data-shape and integration boundaries but cannot reveal a stalled live task. The missing observability was a testability gap, not evidence that a task was currently blocked.
- Recurrence count: one confirmed episode in this repository. This does not justify a global instrumentation rule.
- Proposed minimal action: keep runtime telemetry limited to the explicitly required task loops and keep physical pixels/electrical input as separate manual HIL evidence. Do not generalize this API or add additional task counters without a measured requirement.
- Affected prior tasks: this firmware verification task only; no AGENTS.md, TOOLS.md or skill change is proposed.

Acquisition heartbeat false-positive correction, 2026-09-08:
- Evidence: review found runtime_observability_beat() was called before potentially blocking ad7606_driver_read(); the live HIL heartbeat could advance even while BUSY/SPI read was hung. The call was moved after the read returns on success and error paths; host, sanitizer, coverage, aggregate, and production build checks then passed.
- Root cause: instrumentation was placed at task-loop entry instead of after the blocking hardware boundary.
- Recurrence count: one confirmed episode in this task.
- Proposed minimal action: for future liveness telemetry, place progress/heartbeat updates after each potentially blocking operation and add a regression fixture for a blocked operation if the host seam permits.
- Affected prior tasks: current firmware verification task only.

HIL-gate hardening follow-up, 2026-09-08:
- Evidence: the independent audit found that a missing full-pipeline prerequisite could still reach serial/flash, a replay fixture could carry the wrong configured rate, channel identity/jitter checks were weak, and recording/read-error evidence was under-specified. The corrected runner now stops before hardware, binds log rate to ExpectedSampleRateHz, requires CH1-CH8 diagnostics, exact API IDs and bounded per-frame cadence, exact new session metadata with samples_written>0/dropped_frames=0, and zero acquisition error counters.
- Root cause: the first implementation tested the happy-path presence of each surface but did not bind the checks to the requested configuration and newly-created session, and it treated omitted external prerequisites as a late report state.
- Recurrence count: one confirmed audit episode in this task; no project-wide pattern is established.
- Proposed minimal action: keep full-pipeline prerequisites validated before any side effect and keep replay/live reports explicit about which evidence is physical versus deterministic. Do not generalize to CI/CD or alter AGENTS/TOOLS/skills from this single episode.
- Verification: host/sanitizer/coverage 21/21, pytest 12 passed/1 skipped, no-flash aggregate PASS with HIL SKIPPED, sequential replay PASS/FAIL/BLOCKED exits 0/1/1, and no hardware or external Git action.

Liveness/DMA follow-up, 2026-09-08:
- Evidence: the HIL parser previously accepted a generic SPI3 initialization marker while the live requirement explicitly called for DMA, and recorder heartbeat was emitted before its copy/write/flush boundary. The startup log now states `DMA=auto (SPI_DMA_CH_AUTO)`, replay rejects its absence, and recorder observation occurs after each potentially blocking operation with committed samples_written.
- Root cause: static DMA configuration and a loop-entry heartbeat were weaker than the runtime evidence contract; they could report setup or task entry without proving the relevant operation completed.
- Recurrence count: one confirmed audit episode in this task; no project-wide pattern is established.
- Proposed minimal action: place liveness markers after blocking boundaries and make every runtime acceptance marker encode the configuration being asserted. Keep physical board evidence separate from host/replay tests; do not generalize to CI/CD or edit global instructions.
- Verification: full pytest 14 passed/1 skipped, host/sanitizer/coverage 21/21 each, no-flash aggregate PASS with HIL SKIPPED, production build PASS, and sequential HIL replay PASS/FAIL/BLOCKED exits 0/1/1.

Task-liveness placement follow-up (2026-09-08):
- Evidence: parent review found UI heartbeat at loop entry before `app_event_bus_receive_timeout()` and power-button heartbeat before `vTaskDelay()`, so a blocked receive/delay could still look alive. The implementation moved UI observation after receive/LVGL unlock and power observation after poll delay; static contracts cover acquisition, recorder, UI, keypad and power loops, with focused 5/5 and full 21/21 host verification.
- Root cause: telemetry was initially placed at the loop boundary rather than after the operation whose completion it was meant to prove.
- Recurrence count: one confirmed episode in this repository.
- Proposed minimal action: keep liveness updates after each potentially blocking operation and preserve a small ordering contract test; no global rule or skill change is proposed.

Parallel sanitizer/coverage retry (2026-09-08):
- Evidence: an earlier concurrent host sanitizer and coverage run contended for the shared `build/host-tests` executable and produced a linker `Permission denied`; sequential reruns each completed 21/21 with exit 0.
- Root cause: independent commands wrote the same fixed output paths concurrently.
- Recurrence count: one confirmed orchestration episode.
- Proposed minimal action: run host variants sequentially when they share output directories; no runner or CI/CD policy change is proposed.

WebUI boundary follow-up (2026-09-08):
- Evidence: `fetchSeismic()` previously trusted an unchecked response cast; malformed channel data could reach the waveform renderer. Boundary normalization plus signed-impulse and rejection/recovery regressions now pass 4/4 focused, 26/26 console and typecheck.
- Root cause: API shape validation was deferred past the transport boundary.
- Recurrence count: one confirmed episode in this repository.
- Proposed minimal action: validate external JSON at the API boundary and keep live Wi-Fi/browser/pixel claims separate from deterministic tests; no generalized rule is proposed.
