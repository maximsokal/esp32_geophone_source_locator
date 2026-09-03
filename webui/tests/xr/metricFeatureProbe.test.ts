import { describe, expect, it } from 'vitest';

import { metricFeatureProbeRadius } from '../../src/xr/metricFeatureProbe';

function probe(overrides: Partial<Parameters<typeof metricFeatureProbeRadius>[0]> = {}) {
  return metricFeatureProbeRadius({
    depthMeters: 1,
    voxelSizeMeters: 0.04,
    projectionScaleX: 1.5,
    projectionScaleY: 2,
    depthWidth: 640,
    depthHeight: 480,
    ...overrides,
  });
}

describe('metric feature reprobe radius', () => {
  it('keeps the physical support scale stable while image radius shrinks with distance', () => {
    const near = probe({ depthMeters: 1 });
    const far = probe({ depthMeters: 2 });
    expect(near).not.toBeNull();
    expect(far).not.toBeNull();
    expect(near!.targetMeters).toBeCloseTo(far!.targetMeters, 6);
    expect(near!.pixelRadiusX).toBeGreaterThan(far!.pixelRadiusX);
    expect(near!.pixelRadiusY).toBeGreaterThan(far!.pixelRadiusY);
  });

  it('uses XR projection scale so a wider FOV needs fewer pixels for the same metric offset', () => {
    const wide = probe({ projectionScaleX: 0.9 });
    const narrow = probe({ projectionScaleX: 2.2 });
    expect(wide).not.toBeNull();
    expect(narrow).not.toBeNull();
    expect(wide!.targetMeters).toBeCloseTo(narrow!.targetMeters, 6);
    expect(wide!.pixelRadiusX).toBeLessThan(narrow!.pixelRadiusX);
  });

  it('increases the physical step when a coarse depth buffer cannot resolve the nominal voxel fraction', () => {
    const fine = probe({ depthWidth: 640, depthHeight: 480 });
    const coarse = probe({ depthWidth: 40, depthHeight: 30 });
    expect(fine).not.toBeNull();
    expect(coarse).not.toBeNull();
    expect(coarse!.targetMeters).toBeGreaterThan(fine!.targetMeters);
    expect(coarse!.pixelRadiusX).toBeGreaterThanOrEqual(1.25 - 1e-6);
    expect(coarse!.pixelRadiusY).toBeGreaterThanOrEqual(1.25 - 1e-6);
  });

  it('scales the desired support with reconstruction voxel size', () => {
    const fineVoxel = probe({ voxelSizeMeters: 0.025 });
    const coarseVoxel = probe({ voxelSizeMeters: 0.07 });
    expect(fineVoxel).not.toBeNull();
    expect(coarseVoxel).not.toBeNull();
    expect(coarseVoxel!.targetMeters).toBeGreaterThan(fineVoxel!.targetMeters);
  });

  it('refuses a coarse far-depth probe when one resolved pixel would no longer be local', () => {
    expect(probe({
      depthMeters: 6,
      depthWidth: 80,
      depthHeight: 60,
      voxelSizeMeters: 0.04,
    })).toBeNull();
  });

  it('fails closed on malformed metric or projection inputs', () => {
    expect(probe({ depthMeters: Number.NaN })).toBeNull();
    expect(probe({ voxelSizeMeters: 0 })).toBeNull();
    expect(probe({ projectionScaleX: 0 })).toBeNull();
    expect(probe({ depthWidth: 1 })).toBeNull();
  });
});
