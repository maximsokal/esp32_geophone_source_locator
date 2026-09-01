import { describe, expect, it } from 'vitest';

import { ManualControlPointStore } from '../../src/xr/manualControlPoints';
import { OperatorEdgeStore } from '../../src/xr/operatorEdges';
import { projectProjectPointToNormalized } from '../../src/xr/projectionMath';

describe('ManualControlPointStore snapping', () => {
  it('deduplicates repeated taps within the snap radius', () => {
    const store = new ManualControlPointStore();
    const first = store.addOrSnap([1, 2, 3], 0.07);
    const second = store.addOrSnap([1.03, 2.01, 2.99], 0.07);
    expect(first.created).toBe(true);
    expect(second.created).toBe(false);
    expect(second.snapped).toBe(true);
    expect(store.size).toBe(1);
    expect(second.point.id).toBe(first.point.id);
  });

  it('creates a distinct pin when the point is genuinely separate', () => {
    const store = new ManualControlPointStore();
    store.addOrSnap([0, 0, 0]);
    store.addOrSnap([0.2, 0, 0]);
    expect(store.size).toBe(2);
  });
});

describe('OperatorEdgeStore', () => {
  it('deduplicates the same edge in either direction', () => {
    const store = new OperatorEdgeStore();
    const first = store.add([0,0,0],[1,0,0],0.7);
    const second = store.add([1.01,0,0],[0.01,0,0],0.9);
    expect(first).not.toBeNull();
    expect(second?.id).toBe(first?.id);
    expect(store.size).toBe(1);
    expect(store.snapshot()[0].confidence).toBeCloseTo(0.9);
  });

  it('rejects degenerate edges', () => {
    const store = new OperatorEdgeStore();
    expect(store.add([0,0,0],[0.01,0,0])).toBeNull();
    expect(store.size).toBe(0);
  });
});

describe('projectProjectPointToNormalized', () => {
  const identity = Float32Array.from([
    1,0,0,0,
    0,1,0,0,
    0,0,1,0,
    0,0,0,1,
  ]);

  it('projects a project-space point in front of the current XR view', () => {
    // Project +Y maps to WebXR -Z, so [0,1,0] is one metre in front of an
    // identity camera. A point at [0,0,0] lies exactly on the camera plane and
    // must not be considered a visible/manual-placement target.
    const projected = projectProjectPointToNormalized([0,1,0], identity, identity);
    expect(projected).not.toBeNull();
    expect(projected?.u).toBeCloseTo(0.5);
    expect(projected?.v).toBeCloseTo(0.5);
    expect(projected?.viewDepthMeters).toBeCloseTo(1);
    expect(projected?.visible).toBe(true);
  });

  it('rejects points on the camera plane or behind the camera', () => {
    expect(projectProjectPointToNormalized([0,0,0], identity, identity)).toBeNull();
    expect(projectProjectPointToNormalized([0,-1,0], identity, identity)).toBeNull();
  });

  it('rejects invalid transform matrices', () => {
    expect(projectProjectPointToNormalized([0,0,0], new Float32Array(4), identity)).toBeNull();
  });
});
