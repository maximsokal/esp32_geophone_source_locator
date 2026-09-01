import { afterEach, describe, expect, it } from 'vitest';

import {
  deactivateCertifiedGuideRegistry,
  findCertifiedGuideCorner,
  findCertifiedGuideEdgeAtPoint,
  getCertifiedGuideCorners,
  getCertifiedGuideEdges,
  getCertifiedGuideRegistrySummary,
  isCertifiedGuideRegistryActive,
  setCertifiedGuideCorners,
  setCertifiedGuideEdges,
  subscribeCertifiedGuideRegistry,
} from '../../src/xr/certifiedCornerRegistry';
import {
  ManualControlPointStore,
  ManualPlacementRejectedError,
} from '../../src/xr/manualControlPoints';
import { previewManualPlacementFromStableSurface } from '../../src/xr/manualPlacementPreview';
import {
  resetLatestProjectionMatrices,
  setLatestProjectionMatrices,
} from '../../src/xr/latestProjectionRegistry';

const emptySnapshot = {
  positions: new Float32Array(0),
  normals: new Float32Array(0),
  confidence: new Float32Array(0),
  pointCount: 0,
  voxelSizeMeters: 0.04,
};

const simpleProjection = new Float32Array([
  1, 0, 0, 0,
  0, 1, 0, 0,
  0, 0, -1, -1,
  0, 0, -0.2, 0,
]);
const identityWorldFromView = new Float32Array([
  1, 0, 0, 0,
  0, 1, 0, 0,
  0, 0, 1, 0,
  0, 0, 0, 1,
]);

afterEach(() => {
  deactivateCertifiedGuideRegistry();
  resetLatestProjectionMatrices();
});

describe('certified structural registry', () => {
  it('makes an empty active registry a hard manual-placement gate', () => {
    setCertifiedGuideCorners([]);
    expect(isCertifiedGuideRegistryActive()).toBe(true);
    const store = new ManualControlPointStore();
    expect(() => store.addOrSnap([0, 0, 0])).toThrow(ManualPlacementRejectedError);
  });

  it('publishes target availability reactively so UI can stay hidden at 0/0', () => {
    const summaries = [];
    const unsubscribe = subscribeCertifiedGuideRegistry(() => {
      summaries.push(getCertifiedGuideRegistrySummary());
    });
    expect(summaries.at(-1)).toEqual({ active: false, cornerCount: 0, edgeCount: 0, hasTargets: false });

    setCertifiedGuideCorners([]);
    expect(summaries.at(-1)).toEqual({ active: true, cornerCount: 0, edgeCount: 0, hasTargets: false });

    setCertifiedGuideEdges([{
      id: 'edge:reactive',
      start: [-0.5, 2, 0],
      end: [0.5, 2, 0],
      confidence: 0.88,
      support: 24,
      observations: 4,
      source: 'robust-two-plane',
    }]);
    expect(summaries.at(-1)).toEqual({ active: true, cornerCount: 0, edgeCount: 1, hasTargets: true });

    deactivateCertifiedGuideRegistry();
    expect(summaries.at(-1)).toEqual({ active: false, cornerCount: 0, edgeCount: 0, hasTargets: false });
    const countBeforeUnsubscribe = summaries.length;
    unsubscribe();
    setCertifiedGuideCorners([]);
    expect(summaries).toHaveLength(countBeforeUnsubscribe);
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

  it('locks a registry vertex inferred from two mature finite edges without a depth texel at the vertex', () => {
    setCertifiedGuideCorners([{
      id: 'edge-vertex:8:4:2',
      position: [0.48, 0.24, 0.12],
      confidence: 0.86,
      support: 48,
      source: 'multi-edge-vertex',
    }]);

    const preview = previewManualPlacementFromStableSurface(
      0.5,
      0.5,
      emptySnapshot,
      () => ({ u: 0.5, v: 0.5, visible: true, viewDepthMeters: 1.2 }),
    );
    expect(preview.resolution.accepted).toBe(true);
    expect(preview.resolution.mode).toBe('corner');
    expect(preview.resolution.position).toEqual([0.48, 0.24, 0.12]);
    expect(preview.resolution.message).toContain('two mature finite 3D edges');
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

  it('publishes a certified two-plane edge as a manual target before an AUTO endpoint exists', () => {
    setCertifiedGuideCorners([]);
    setCertifiedGuideEdges([{
      id: 'edge:two-plane',
      start: [-0.5, 2, 0],
      end: [0.5, 2, 0],
      confidence: 0.88,
      support: 24,
      observations: 4,
      source: 'robust-two-plane',
    }]);
    expect(getCertifiedGuideCorners()).toHaveLength(0);
    expect(getCertifiedGuideEdges()).toHaveLength(1);
    expect(findCertifiedGuideEdgeAtPoint([0, 2, 0])).not.toBeNull();
  });

  it('lets the camera ray supply the missing coordinate along a certified edge', () => {
    setCertifiedGuideCorners([]);
    setCertifiedGuideEdges([{
      id: 'edge:two-plane',
      start: [-0.5, 2, 0],
      end: [0.5, 2, 0],
      confidence: 0.90,
      support: 24,
      observations: 4,
      source: 'robust-two-plane',
    }]);
    setLatestProjectionMatrices(simpleProjection, identityWorldFromView);

    const preview = previewManualPlacementFromStableSurface(
      0.5,
      0.5,
      emptySnapshot,
      (point) => ({
        u: 0.5 + point[0] / 4,
        v: 0.5,
        visible: true,
        viewDepthMeters: point[1],
      }),
    );

    expect(preview.resolution.accepted).toBe(true);
    expect(preview.resolution.mode).toBe('edge');
    expect(preview.resolution.position).not.toBeNull();
    expect(preview.resolution.position![0]).toBeCloseTo(0, 5);
    expect(preview.resolution.position![1]).toBeCloseTo(2, 5);
    expect(preview.resolution.position![2]).toBeCloseTo(0, 5);

    const store = new ManualControlPointStore();
    const stored = store.addOrSnap(preview.resolution.position!);
    expect(stored.point.position[1]).toBeCloseTo(2, 5);
    expect(stored.placementMode).toBe('edge');
  });
});
