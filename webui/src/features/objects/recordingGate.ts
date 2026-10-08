import type { RecordingGateStatus } from '../../domain/device';

/**
 * A recording gate only prevents admission of a new session. It must never
 * disable the stop action for the session that is already selected.
 */
export function recordingGateBlocksStart(
  gate: RecordingGateStatus | undefined,
  selectedRecording: boolean,
): boolean {
  return !selectedRecording && gate?.available === true && gate.allowed === false;
}

export function recordingStartUnavailable(
  recordingAvailable: boolean | undefined,
  storageAvailable: boolean | undefined,
  selectedRecording: boolean,
): boolean {
  return !selectedRecording &&
    (recordingAvailable === false || storageAvailable === false);
}
