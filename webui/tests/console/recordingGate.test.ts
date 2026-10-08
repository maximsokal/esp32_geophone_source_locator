import { describe, expect, it } from 'vitest';
import type { RecordingGateStatus } from '../../src/domain/device';
import {
  recordingGateBlocksStart,
  recordingStartUnavailable,
} from '../../src/features/objects/recordingGate';
import { recordingGateReason } from '../../src/features/objects/recordingGateUi';

function gate(overrides: Partial<RecordingGateStatus> = {}): RecordingGateStatus {
  return {
    available: true,
    assigned_count: 3,
    accepted_count: 3,
    decision: 'ALLOWED_3D_CAPABLE',
    dimensionality: 'CAPABLE_3D',
    allowed: true,
    reason: '',
    warning: '',
    ...overrides,
  };
}

describe('recording gate admission', () => {
  it('blocks a new session when the device explicitly rejects admission', () => {
    expect(recordingGateBlocksStart(
      gate({
        assigned_count: 3,
        accepted_count: 2,
        decision: 'BLOCKED_NOT_LEVEL',
        allowed: false,
      }),
      false,
    )).toBe(true);
  });

  it('does not block stopping the selected active recording', () => {
    expect(recordingGateBlocksStart(
      gate({ decision: 'BLOCKED_INSUFFICIENT_REMOTES', allowed: false }),
      true,
    )).toBe(false);
  });

  it('does not block when the gate is unavailable', () => {
    expect(recordingGateBlocksStart(
      gate({ available: false, allowed: false, error: 'ESP_ERR_INVALID_STATE' }),
      false,
    )).toBe(false);
    expect(recordingGateBlocksStart(undefined, false)).toBe(false);
  });

  it('allows both planar and 3D-capable admissions', () => {
    expect(recordingGateBlocksStart(
      gate({ decision: 'ALLOWED_PLANAR_2D', dimensionality: 'PLANAR_2D', allowed: true }),
      false,
    )).toBe(false);
    expect(recordingGateBlocksStart(
      gate({ decision: 'ALLOWED_3D_CAPABLE', dimensionality: 'CAPABLE_3D', allowed: true }),
      false,
    )).toBe(false);
  });

  it('disables new recording when storage or the recorder is unavailable', () => {
    expect(recordingStartUnavailable(false, true, false)).toBe(true);
    expect(recordingStartUnavailable(true, false, false)).toBe(true);
    expect(recordingStartUnavailable(undefined, undefined, false)).toBe(false);
    expect(recordingStartUnavailable(false, false, true)).toBe(false);
  });

  it('prefers the storage-unavailable reason code over an eligible topology decision', () => {
    expect(recordingGateReason(
      gate({ reason: 'STORAGE_UNAVAILABLE' }),
      (key) => key,
    )).toBe('recording_gate.reason.storage_unavailable');
  });
});
