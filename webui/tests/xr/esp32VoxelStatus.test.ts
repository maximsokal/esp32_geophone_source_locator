import { describe, expect, it } from 'vitest';

import { esp32VoxelPreviewState } from '../../src/xr/esp32VoxelStatus';
import type { PreparedDerivatives, ScanPersistenceState } from '../../src/xr/scanPersistence';
import { encodeVoxelPreviewArtifact } from '../../src/xr/voxelPreview';

const previewBytes = encodeVoxelPreviewArtifact({
  sourceScanCrc32: 7, geometryComputedAtMs: 0n, registration: null, origin: [0, 0, 0], voxelSizeMeters: 0.25, dimensions: [2, 1, 1],
  voxelCount: 2, indexWidth: 2, indices: new Uint32Array([0, 1]), faceMasks: new Uint8Array([63, 63]), coarseningSteps: 0,
});
const prepared = (overrides: Partial<PreparedDerivatives> = {}): PreparedDerivatives =>
  ({ objectId: 'OBJ', sessionId: 'staging', scanId: 'scan-crc32:00000007', occupancy: null, preview: previewBytes, ...overrides });
const persistence = (overrides: Partial<ScanPersistenceState> = {}): ScanPersistenceState => ({
  scan: 'saved', occupancy: 'ready', preview: 'pending', scanError: null, occupancyError: null, previewError: null,
  scanId: 'scan-crc32:00000007', outcome: 'confirmed_committed', previousKept: false, ...overrides,
});

describe('ESP32 voxel preview state (issue #44): the status never claims more than is verified', () => {
  it('has nothing to show until the preview bytes exist', () => {
    expect(esp32VoxelPreviewState(null, null)).toBeNull();
    expect(esp32VoxelPreviewState(prepared({ preview: null, occupancy: new Uint8Array([1]) }), null)).toBeNull();
  });

  it('decodes the exact bytes and calls them only "prepared" before any upload is confirmed', () => {
    const state = esp32VoxelPreviewState(prepared(), null)!;
    expect(state.artifact?.voxelCount).toBe(2);
    expect(state.status).toBe('prepared');
    expect(esp32VoxelPreviewState(prepared(), persistence({ scan: 'saving', preview: 'pending' }))!.status).toBe('prepared');
    expect(esp32VoxelPreviewState(prepared(), persistence({ scan: 'verifying' }))!.status).toBe('prepared');
    expect(esp32VoxelPreviewState(prepared(), persistence({ scan: 'blocked_recording' }))!.status).toBe('prepared');
    expect(esp32VoxelPreviewState(prepared(), persistence({ scan: 'error', outcome: 'confirmed_not_committed' }))!.status).toBe('prepared');
  });

  it('follows the preview upload of THIS scan: uploading, then saved only once the device accepted it', () => {
    expect(esp32VoxelPreviewState(prepared(), persistence({ preview: 'pending' }))!.status).toBe('prepared');
    expect(esp32VoxelPreviewState(prepared(), persistence({ preview: 'saving' }))!.status).toBe('uploading');
    expect(esp32VoxelPreviewState(prepared(), persistence({ preview: 'ready' }))!.status).toBe('saved');
    expect(esp32VoxelPreviewState(prepared(), persistence({ preview: 'failed', previewError: 'timeout' }))!.status).toBe('failed');
    expect(esp32VoxelPreviewState(prepared(), persistence({ preview: 'stale' }))!.status).toBe('stale');
    expect(esp32VoxelPreviewState(prepared(), persistence({ preview: 'unavailable' }))!.status).toBe('unavailable');
  });

  it('ignores the persistence state of another scan: it says nothing about these bytes', () => {
    expect(esp32VoxelPreviewState(prepared(), persistence({ scanId: 'scan-crc32:deadbeef', preview: 'ready' }))!.status).toBe('prepared');
  });

  it('reports a corrupt artifact instead of drawing it', () => {
    const corrupt = previewBytes.slice();
    corrupt[corrupt.length - 1] ^= 0xff;
    const state = esp32VoxelPreviewState(prepared({ preview: corrupt }), null)!;
    expect(state.artifact).toBeNull();
    expect(state.error).not.toBeNull();
  });
});
