import { afterEach, describe, expect, it } from 'vitest';

import {
  deactivateCertifiedGuideRegistry,
  findCertifiedGuideCorner,
  getCertifiedGuideCorners,
  isCertifiedGuideRegistryActive,
  setCertifiedGuideCorners,
} from '../../src/xr/certifiedCornerRegistry';
import {
  ManualControlPointStore,
  ManualPlacementRejectedError,
} from '../../src/xr/manualControlPoints';
import { previewManualPlacementFromStableSurface } from '../../src/xr/manualPlacementPreview';

const emptySnapshot = {
  positions: new Float32Array(0),
  normals: new Float32Array(0),
  confidence: new Float32Array(0),
  pointCount: 0,
  voxelSizeMeters: 0.04,
};

afterEach(() => deactivateCertifiedGuideRegistry());

describe('certified corner registry', () => {
  it('makes an empty active registry a hard manual-placement gate', () => {
    setCertifiedGuideCorners([]);
    expect(isCertifiedGuideRegistryActive()).toBe(true);
    const store = new ManualControlPointStore();
    expect(() => store.addOrSnap([0, 0, 0])).toThrow(ManualPlacementRejectedError);
  });

  it('shares exactly the same certified target with manual preview and storage', () => {
    setCertifiedGuideCorners([{
      id: 'robust:0:0:0',
      position: [0.10, 0.20, 0.30],
      confidence: 0.84,
      support: 18,
      source: 'three-plane',
    }]);

    const preview = previewManualPlacementFromStableSurface(
      0.5,
      0.5,
      emptySnapshot,
      (point) => ({
        u: 0.5,
        v: 0.5,
        visible: true,
        viewDepthMeters: point[2] + 1,
      }),
    );
    expect(preview.resolution.accepted).toBe(true);
    expect(preview.resolution.mode).toBe('corner');
    expect(preview.resolution.position).toEqual([0.10, 0.20, 0.30]);

    const store = new ManualControlPointStore();
    const stored = store.addOrSnap(preview.resolution.position!);
    expect(stored.point.position).toEqual([0.10, 0.20, 0.30]);
    expect(stored.snapped).toBe(true);
  });

  it('does not allow a raw point near the scene to bypass the certified target', () => {
    setCertifiedGuideCorners([{
      id: 'endpoint:1',
      position: [0.20, 0.10, 0.40],
      confidence: 0.81,
      support: 14,
      source: 'multi-view-edge-endpoint',
    }]);
    expect(findCertifiedGuideCorner([0.20, 0.10, 0.40])).not.toBeNull();
    const store = new ManualControlPointStore();
    expect(() => store.addOrSnap([0.28, 0.10, 0.40])).toThrow(ManualPlacementRejectedError);
    expect(getCertifiedGuideCorners()).toHaveLength(1);
  });
});
