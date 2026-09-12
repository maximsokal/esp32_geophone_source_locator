# Candidate retrospective: Wi-Fi network-event race follow-up

- Evidence: the first focused Python contract run failed at the new ordering assertion because the test selected the earlier forward declaration of `web_server_mode_transition_complete()` and produced an empty source slice. The implementation was not implicated; the slice was narrowed to search after the new helper, and the rerun passed 20/20.
- Root cause: source-contract extraction used an unbounded `index()` for a repeated function declaration instead of anchoring the end marker after the selected start.
- Recurrence count: 1 confirmed episode in this task; insufficient evidence for a generalized rule.
- Proposed minimal rule/tool change: none yet. If the same repeated-declaration slicing error occurs again, add a test helper that always searches the end marker from the selected start before proposing a broader rule.
- Affected prior tasks: none confirmed.
