import { describe, expect, it } from 'vitest';

import { detectWeakSurfaceBridgeEvidence } from '../../src/xr/surfaceBridgeEvidence';

function flatten(points: readonly (readonly [number, number, number])[]): Float32Array {
  return Float32Array.from(points.flatMap((point) => point));
}

describe('weak physical bridge evidence', () => {
  it('marks a quarantined island for rescan when multiple weak surfels connect it to retained structure', () => {
    const result = detectWeakSurfaceBridgeEvidence({
      positions: flatten([
        [0.00, 0, 0], [0.02, 0, 0],
        [0.06, 0, 0], [0.12, 0, 0],
        [0.18, 0, 0], [0.20, 0, 0],
      ]),
      componentIdByPoint: Int32Array.from([0, 0, -1, -1, 1, 1]),
      retainedComponentIds: new Set([0]),
      quarantinedComponentIds: new Set([1]),
      voxelSizeMeters: 0.04,
    });
    expect(result.rescanComponentIds).toEqual([1]);
    expect(result.bridgeClusterCount).toBe(1);
    expect(result.truncated).toBe(false);
  });

  it('does not invent a bridge when weak points touch only the quarantined island', () => {
    const result = detectWeakSurfaceBridgeEvidence({
      positions: flatten([
        [0.00, 0, 0], [0.02, 0, 0],
        [0.30, 0, 0], [0.34, 0, 0],
        [0.38, 0, 0], [0.40, 0, 0],
      ]),
      componentIdByPoint: Int32Array.from([0, 0, -1, -1, 1, 1]),
      retainedComponentIds: new Set([0]),
      quarantinedComponentIds: new Set([1]),
      voxelSizeMeters: 0.04,
    });
    expect(result.rescanComponentIds).toEqual([]);
    expect(result.bridgeClusterCount).toBe(0);
  });

  it('requires more than one weak surfel before requesting a bridge rescan', () => {
    const result = detectWeakSurfaceBridgeEvidence({
      positions: flatten([
        [0.00, 0, 0], [0.02, 0, 0],
        [0.075, 0, 0],
        [0.13, 0, 0], [0.15, 0, 0],
      ]),
      componentIdByPoint: Int32Array.from([0, 0, -1, 1, 1]),
      retainedComponentIds: new Set([0]),
      quarantinedComponentIds: new Set([1]),
      voxelSizeMeters: 0.04,
    });
    expect(result.rescanComponentIds).toEqual([]);
  });

  it('is sensor-neutral and reports only component ids already supplied by quarantine policy', () => {
    const result = detectWeakSurfaceBridgeEvidence({
      positions: flatten([
        [0.00, 0, 0], [0.06, 0, 0], [0.12, 0, 0], [0.18, 0, 0],
      ]),
      componentIdByPoint: Int32Array.from([0, -1, -1, 99]),
      retainedComponentIds: new Set([0]),
      quarantinedComponentIds: new Set([1]),
      voxelSizeMeters: 0.04,
    });
    expect(result.rescanComponentIds).toEqual([]);
  });
});
