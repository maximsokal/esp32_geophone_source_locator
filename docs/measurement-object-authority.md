# Measurement object authority

The measurement service owns the identity of the object attached to an accepted measurement session.

## Session owner

Once START resolves a concrete object, `measurement_service_snapshot_t.object_id` and `object_name` describe that measurement/session owner. The identity is immutable through `RECORDING`, `STOPPING`, `REVIEW`, `DELETING`, and object-bound `ERROR` states. A later change of the globally ACTIVE object does not migrate or relabel the current/reviewed session.

`seismic_recorder_snapshot_t.object_id` and the measurement session owner must therefore agree while a recorder session is active. REVIEW may coexist with a different globally ACTIVE object because the reviewed session is already closed for recording and the new ACTIVE selection is only the default for a future START.

Successful KEEP/DELETE and the beginning of a new START clear stale session identity before a future owner is resolved. In idle `LIVE`, and in an `ERROR` that occurred before any concrete owner was captured, the public measurement snapshot projects the current globally ACTIVE object for the next START/retry.

`active_object_present` remains a separate statement about whether a current global ACTIVE object exists. It does not redefine the session owner fields.

## Object-mutation admission

Web and local TFT object controls share `measurement_service_snapshot_allows_object_mutation()`.

Object mutations are admitted only when the measurement snapshot is not busy and the state is one of:

- `MEASUREMENT_SERVICE_LIVE`
- `MEASUREMENT_SERVICE_REVIEW`
- `MEASUREMENT_SERVICE_ERROR`

They are rejected during `STARTING`, `RECORDING`, `STOPPING`, or `DELETING`, and during any submit-gate busy window. Local TFT controls reject immediately and do not queue a delayed NEW / SET ACTIVE / DELETE operation.

This is an application-level UX/admission rule. It does not replace lower storage safety. In particular, MAX-58 remains responsible for authoritative protection against deleting an object that still has open recorder/scan file lifetimes.
