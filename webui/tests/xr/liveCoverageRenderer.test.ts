import { describe, expect, it } from 'vitest';

import { applyCoverageRenderBudget } from '../../src/xr/liveCoverageRenderer';

function snapshot(regular: number, manual = 0) {
  const count = regular + manual;
  const positions = new Float32Array(count * 3);
  const confidence = new Float32Array(count);
  for (let index = 0; index < count; index += 1) {
    positions[index * 3] = index;
    positions[index * 3 + 1] = index + 0.25;
    positions[index * 3 + 2] = index + 0.5;
    confidence[index] = index < regular ? 0.2 + (index % 7) * 0.1 : 2;
  }
  return { positions, confidence, pointCount: count, manualPointCount: manual };
}

describe('applyCoverageRenderBudget', () => {
  it('does not touch full-detail snapshots below the budget', () => {
    const input = snapshot(100, 2);
    const output = applyCoverageRenderBudget(input, 12000);
    expect(output.pointCount).toBe(102);
    expect(output.manualPointCount).toBe(2);
    expect(Array.from(output.positions)).toEqual(Array.from(input.positions));
  });

  it('downsamples only regular live points and always preserves manual pins', () => {
    const input = snapshot(12000, 3);
    const output = applyCoverageRenderBudget(input, 4800);
    expect(output.pointCount).toBe(4803);
    expect(output.manualPointCount).toBe(3);
    for (let manual = 0; manual < 3; manual += 1) {
      const target = 4800 + manual;
      const source = 12000 + manual;
      expect(output.positions[target * 3]).toBe(input.positions[source * 3]);
      expect(output.confidence[target]).toBe(2);
    }
  });

  it('is deterministic so pressure changes cannot reshuffle the same LOD randomly', () => {
    const input = snapshot(1000);
    const a = applyCoverageRenderBudget(input, 137);
    const b = applyCoverageRenderBudget(input, 137);
    expect(Array.from(a.positions)).toEqual(Array.from(b.positions));
    expect(Array.from(a.confidence)).toEqual(Array.from(b.confidence));
  });

  it('supports a zero regular-point budget without deleting operator pins', () => {
    const input = snapshot(30, 2);
    const output = applyCoverageRenderBudget(input, 0);
    expect(output.pointCount).toBe(2);
    expect(output.manualPointCount).toBe(2);
    expect(Array.from(output.confidence)).toEqual([2, 2]);
  });
});
