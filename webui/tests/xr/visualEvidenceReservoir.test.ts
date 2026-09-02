import { describe, expect, it } from 'vitest';

import { PoseDiverseVisualEvidenceStore } from '../../src/xr/visualEvidenceReservoir';
import type { VisualEvidenceFrame } from '../../src/xr/visualLineEvidence';

function frame(keyframeId: number, x: number, capturedAtMs = keyframeId * 100): VisualEvidenceFrame {
  return {
    keyframeId,
    capturedAtMs,
    projectionMatrix: new Float32Array([
      1,0,0,0,
      0,1,0,0,
      0,0,0,-1,
      0,0,0,0,
    ]),
    worldFromView: new Float32Array([
      1,0,0,0,
      0,1,0,0,
      0,0,1,0,
      x,0,0,1,
    ]),
    lines: [{
      id: `line:${keyframeId}:${capturedAtMs}`,
      start: [0.2,0.5],
      end: [0.8,0.5],
      direction: [1,0],
      length: 0.6,
      strength: 0.9,
      supportPixels: 40,
    }],
    junctions: [],
  };
}

describe('pose-diverse visual evidence reservoir', () => {
  it('evicts one near-duplicate pose before scan-wide distinct viewpoints', () => {
    const store = new PoseDiverseVisualEvidenceStore(3);
    store.add(frame(1, 0.00));
    store.add(frame(2, 0.01));
    store.add(frame(3, 0.80));
    store.add(frame(4, 1.60));

    expect(store.size).toBe(3);
    const ids = store.snapshot().map((item) => item.keyframeId);
    expect(ids).toContain(3);
    expect(ids).toContain(4);
    expect(ids.filter((id) => id === 1 || id === 2)).toHaveLength(1);
  });

  it('remains hard-bounded while preserving both early and late viewpoints on a long scan path', () => {
    const store = new PoseDiverseVisualEvidenceStore(8);
    for (let keyframeId = 1; keyframeId <= 100; keyframeId += 1) {
      store.add(frame(keyframeId, keyframeId * 0.03));
    }
    expect(store.size).toBe(8);
    const retained = store.snapshot();
    expect(retained.some((item) => item.capturedAtMs <= 2000)).toBe(true);
    expect(retained.some((item) => item.capturedAtMs >= 8000)).toBe(true);
  });

  it('keeps different RGB captures even when async delivery labels them with the same depth keyframe', () => {
    const store = new PoseDiverseVisualEvidenceStore(4);
    store.add(frame(10, 0.00, 1000));
    store.add(frame(10, 0.20, 1520));

    const retained = store.snapshot();
    expect(retained).toHaveLength(2);
    expect(retained.map((item) => item.capturedAtMs)).toEqual([1000, 1520]);
  });

  it('replaces duplicate processing of the same RGB capture even if it arrives under another keyframe id', () => {
    const store = new PoseDiverseVisualEvidenceStore(4);
    store.add(frame(10, 0.00, 1000));
    store.add(frame(11, 0.00, 1000));

    const retained = store.snapshot();
    expect(retained).toHaveLength(1);
    expect(retained[0].keyframeId).toBe(11);
    expect(retained[0].capturedAtMs).toBe(1000);
  });
});
