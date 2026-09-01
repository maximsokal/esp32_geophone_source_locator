import { describe, expect, it } from 'vitest';

import { PoseDiverseVisualEvidenceStore } from '../../src/xr/visualEvidenceReservoir';
import type { VisualEvidenceFrame } from '../../src/xr/visualLineEvidence';

function frame(keyframeId: number, x: number): VisualEvidenceFrame {
  return {
    keyframeId,
    capturedAtMs: keyframeId * 100,
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
      id: `line:${keyframeId}`,
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
    expect(retained.some((item) => item.keyframeId <= 20)).toBe(true);
    expect(retained.some((item) => item.keyframeId >= 80)).toBe(true);
  });
});