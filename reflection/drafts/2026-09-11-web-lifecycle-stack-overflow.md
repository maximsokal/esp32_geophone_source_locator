# Candidate retrospective: web lifecycle on `sys_evt`

- Evidence: hardware logs showed `sys_evt` stack overflow immediately after `MEM_RUNTIME stage=before-https`; production configuration had `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=2304`. The IP callback chain synchronously initialized mDNS, started HTTPS, and ran `runtime_observability_log_snapshot()`.
- Root cause: network event callbacks were treated as lifecycle workers. The callback context was not treated as a constrained system task, so TLS setup and a full FreeRTOS census shared its small stack.
- Correction: network callbacks now enqueue a coalescing lifecycle command; a dedicated service-core PSRAM task owns mDNS, HTTP/HTTPS transitions, and the full HTTPS setup path. HTTPS diagnostics request snapshots asynchronously. A focused static contract and the target build protect the boundary.
- Recurrence count: one confirmed occurrence in this task; prior audits repeatedly found other synchronous work crossing constrained task boundaries, but no second confirmed `sys_evt` stack-overflow episode.
- Minimal proposed rule: classify ESP-IDF event callbacks as notification-only by default; require an explicit task/stack justification for any blocking, allocation-heavy, TLS, filesystem, or full-diagnostics call in them.
- Affected prior work: runtime observability, WebUI cache/startup, LVGL scheduling, and Wi-Fi/Web server startup reviews in the 2026-09-11 working log.
