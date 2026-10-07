import { describe, expect, it } from 'vitest';

import {
  MANUAL_ANNOTATION_CAPACITY_MESSAGE,
  MAX_MANUAL_CONTROL_POINTS,
  ManualControlPointStore,
  projectDepthSampleToProject,
  projectPointToXrReference,
} from '../../src/xr/manualControlPoints';

const identity = new Float32Array([
  1, 0, 0, 0,
  0, 1, 0, 0,
  0, 0, 1, 0,
  0, 0, 0, 1,
]);

describe('manual depth projection', () => {
  it('projects the center depth sample into project X/Y horizontal + Z height coordinates', () => {
    const point = projectDepthSampleToProject({
      u: 0.5,
      v: 0.5,
      depthMeters: 2,
      projectionMatrix: identity,
      worldFromView: identity,
    });
    expect(point).not.toBeNull();
    expect(point![0]).toBeCloseTo(0, 6);
    expect(point![1]).toBeCloseTo(2, 6);
    expect(point![2]).toBeCloseTo(0, 6);
  });

  it('preserves WebXR world translation before converting coordinate convention', () => {
    const translated = new Float32Array(identity);
    translated[12] = 1.25;
    translated[13] = 0.4;
    translated[14] = -0.75;
    const point = projectDepthSampleToProject({
      u: 0.5,
      v: 0.5,
      depthMeters: 2,
      projectionMatrix: identity,
      worldFromView: translated,
    });
    expect(point).not.toBeNull();
    expect(point![0]).toBeCloseTo(1.25, 6);
    expect(point![1]).toBeCloseTo(2.75, 6);
    expect(point![2]).toBeCloseTo(0.4, 6);
  });

  it('rejects invalid depth or projection matrices', () => {
    expect(projectDepthSampleToProject({
      u: 0.5,
      v: 0.5,
      depthMeters: Number.NaN,
      projectionMatrix: identity,
      worldFromView: identity,
    })).toBeNull();
    expect(projectDepthSampleToProject({
      u: 0.5,
      v: 0.5,
      depthMeters: 1,
      projectionMatrix: new Float32Array(4),
      worldFromView: identity,
    })).toBeNull();
  });

  it('round-trips project positions into the active WebXR reference convention', () => {
    expect(projectPointToXrReference([1, 2, 3])).toEqual([1, 3, -2]);
  });
});

describe('manual control point store', () => {
  it('adds, moves, undoes and resets operator pins deterministically', () => {
    const store = new ManualControlPointStore();
    const first = store.add([1, 2, 3]);
    const second = store.add([4, 5, 6]);
    expect(first.id).toBe(1);
    expect(second.id).toBe(2);
    expect(store.size).toBe(2);
    expect(store.lastId).toBe(2);

    expect(store.update(second.id, [7, 8, 9])?.position).toEqual([7, 8, 9]);
    expect(store.update(999, [0, 0, 0])).toBeNull();
    expect(Array.from(store.xrReferencePositions())).toEqual([1, 3, -2, 7, 9, -8]);

    expect(store.removeLast()?.id).toBe(2);
    expect(store.lastId).toBe(1);
    expect(store.size).toBe(1);

    store.reset();
    expect(store.size).toBe(0);
    expect(store.lastId).toBeNull();
    expect(store.snapshot()).toEqual([]);
  });

  it('returns snapshot copies that cannot mutate stored points', () => {
    const store = new ManualControlPointStore();
    store.add([1, 2, 3]);
    const snapshot = store.snapshot();
    const position = snapshot[0].position as [number, number, number];
    position[0] = 99;
    expect(store.snapshot()[0].position[0]).toBe(1);
  });

  it('keeps the store bounded while allowing a snap to update a pin at capacity', () => {
    const store = new ManualControlPointStore();
    for (let index = 0; index < MAX_MANUAL_CONTROL_POINTS; index += 1) store.add([index, 0, 0]);

    const snapped = store.addCertifiedOrSnap([0.02, 0, 0], 0.9, 0.065);
    expect(snapped.created).toBe(false);
    expect(store.size).toBe(MAX_MANUAL_CONTROL_POINTS);
    expect(store.capacityDiagnostics).toEqual({ count: MAX_MANUAL_CONTROL_POINTS, maximum: MAX_MANUAL_CONTROL_POINTS, capacityReached: true });
    expect(() => store.add([1000, 0, 0])).toThrow(MANUAL_ANNOTATION_CAPACITY_MESSAGE);
    expect(store.size).toBe(MAX_MANUAL_CONTROL_POINTS);
  });
});
