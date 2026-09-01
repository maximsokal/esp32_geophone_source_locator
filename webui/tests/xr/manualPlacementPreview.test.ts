import { describe, expect, it } from 'vitest';

import {
  manualPreviewLabel,
  previewManualPlacementFromStableSurface,
} from '../../src/xr/manualPlacementPreview';
import type { ManualPlacementSurfaceSnapshot } from '../../src/xr/manualPlacementSurface';

interface MutableCloud {
  positions: number[];
  normals: number[];
  confidence: number[];
}

function cloud(): MutableCloud {
  return { positions: [], normals: [], confidence: [] };
}

function add(
  target: MutableCloud,
  position: readonly [number, number, number],
  normal: readonly [number, number, number],
): void {
  target.positions.push(...position);
  target.normals.push(...normal);
  target.confidence.push(0.94);
}

function snapshot(source: MutableCloud): ManualPlacementSurfaceSnapshot {
  return {
    positions: Float32Array.from(source.positions),
    normals: Float32Array.from(source.normals),
    confidence: Float32Array.from(source.confidence),
    pointCount: source.confidence.length,
    voxelSizeMeters: 0.04,
  };
}

function addPlaneX(target: MutableCloud, zOffset = 0): void {
  for (const y of [-0.08, -0.04, 0, 0.04, 0.08]) {
    for (const z of [-0.06, -0.02, 0.02, 0.06]) add(target, [0, y, z + zOffset], [1, 0, 0]);
  }
}

function addPlaneY(target: MutableCloud, zOffset = 0): void {
  for (const x of [-0.08, -0.04, 0, 0.04, 0.08]) {
    for (const z of [-0.06, -0.02, 0.02, 0.06]) add(target, [x, 0, z + zOffset], [0, 1, 0]);
  }
}

function addPlaneZ(target: MutableCloud): void {
  for (const x of [-0.08, -0.04, 0, 0.04, 0.08]) {
    for (const y of [-0.06, -0.02, 0.02, 0.06]) add(target, [x, y, 0], [0, 0, 1]);
  }
}

const projector = (point: readonly [number, number, number]) => ({
  u: 0.5 + point[0],
  v: 0.5 + point[1],
  visible: true,
});

describe('manual placement live preview', () => {
  it('shows EDGE LOCK only where projected stable multi-view geometry exists', () => {
    const source = cloud();
    addPlaneX(source);
    addPlaneY(source);

    const preview = previewManualPlacementFromStableSurface(0.5, 0.5, snapshot(source), projector);
    expect(preview.resolution.accepted).toBe(true);
    expect(preview.resolution.mode).toBe('edge');
    expect(manualPreviewLabel(preview)).toBe('EDGE LOCK');
  });

  it('shows CORNER LOCK for a conditioned three-plane accumulated corner', () => {
    const source = cloud();
    addPlaneX(source);
    addPlaneY(source);
    addPlaneZ(source);

    const preview = previewManualPlacementFromStableSurface(0.5, 0.5, snapshot(source), projector);
    expect(preview.resolution.accepted).toBe(true);
    expect(preview.resolution.mode).toBe('corner');
    expect(preview.resolution.confidence).toBeLessThan(1);
    expect(manualPreviewLabel(preview)).toBe('CORNER LOCK');
  });

  it('shows SCAN MORE when the cursor is away from stable projected geometry', () => {
    const source = cloud();
    addPlaneX(source);
    addPlaneY(source);

    const preview = previewManualPlacementFromStableSurface(0.9, 0.9, snapshot(source), projector);
    expect(preview.resolution.accepted).toBe(false);
    expect(preview.seedPosition).toBeNull();
    expect(manualPreviewLabel(preview)).toBe('SCAN MORE');
  });

  it('does not offer a corner when only one stable surface family is known', () => {
    const source = cloud();
    addPlaneX(source);

    const preview = previewManualPlacementFromStableSurface(0.5, 0.5, snapshot(source), projector);
    expect(preview.resolution.accepted).toBe(false);
    expect(manualPreviewLabel(preview)).toBe('SCAN MORE');
  });

  it('selects the frontmost stable surface when foreground and background overlap on screen', () => {
    const source = cloud();
    addPlaneX(source, 0);
    addPlaneY(source, 0);
    addPlaneX(source, 0.55);
    addPlaneY(source, 0.55);

    const preview = previewManualPlacementFromStableSurface(
      0.5,
      0.5,
      snapshot(source),
      (point) => ({
        u: 0.5 + point[0],
        v: 0.5 + point[1],
        visible: true,
        viewDepthMeters: 1 + point[2],
      }),
    );

    expect(preview.seedPosition).not.toBeNull();
    expect(preview.seedViewDepthMeters).not.toBeNull();
    expect(preview.seedViewDepthMeters!).toBeLessThan(1.2);
    expect(preview.resolution.accepted).toBe(true);
    expect(preview.resolution.position![2]).toBeLessThan(0.2);
  });
});
