# Wi-Fi transition cleanup uncertainty

## Evidence

- Review of `59e96cf` identified that a failed target-mode start followed by a failed cleanup `esp_wifi_stop()` still led to `running=false` and `setup_mode=false` without an uncertainty marker.
- The same review identified that authoritative Web lifecycle completion used a zero-timeout mutex acquisition, so a transient contention could lose the final reconciliation.
- Host C now exercises the sequential stop/start/cleanup fault path and recovery; the policy test asserts the blocking setter and uncertainty plumbing.

## Root cause

The transition code treated the software fallback flags as proof that the radio had stopped, even when the cleanup operation had failed. The Web completion callback reused an event-oriented try-lock helper for a state that must be delivered.

## Minimal fix

Carry the cleanup result through the completion callback, store a dedicated `radio_state_uncertain` flag, reject new mode starts until an explicit stop retries cleanup, and use a blocking authoritative lifecycle setter for the final Web state. Keep the mutex-held section limited to state/queue bookkeeping.

## Recurrence count

1 confirmed occurrence in this task. This is not yet enough evidence for a generalized repository-wide rule.

## Proposed rule/tool change

After changing a callback signature, run the production compilation before declaring the change verified and search for every forward declaration/call site. For future transition reviews, require separate tests for primary failure, cleanup failure, and authoritative completion delivery.

## Affected work

The current Wi-Fi mode-transition and Web lifecycle changes descended from the `93cbcb31`/`59e96cf` transition series. No existing architecture, storage format, pins, or hardware state was changed.
