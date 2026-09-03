import { describe, expect, it } from 'vitest';

import type { ScanCapabilityReadiness } from '../../src/xr/scanCapabilityReadiness';
import { compactScanReadinessLabel } from '../../src/xr/scanReadinessUi';

function readiness(overrides: Partial<ScanCapabilityReadiness> = {}): ScanCapabilityReadiness {
  return {
    structural: { progress: 0.74, ready: false, limitingMetric: 'extent', growthNeededPercent: 35 },
    mesh: { progress: 0.52, ready: false, limitingMetric: 'extent', growthNeededPercent: 92 },
    viewCoverage: 0.3125,
    viewSectors: 5,
    totalViewSectors: 16,
    cameraBaselineMeters: 0.18,
    objectMajorExtentMeters: 0.10,
    objectMinorExtentMeters: 0.04,
    objectTooSmall: true,
    ...overrides,
  };
}

describe('compact scan readiness UI', () => {
  it('shows measured camera azimuth sectors, feature maturity and honest linear size growth once an object component exists', () => {
    expect(compactScanReadinessLabel(readiness(), {
      visibleCorners: 2,
      certifiedCorners: 4,
      certifiedEdges: 1,
      planarPatches: 3,
      workerEdges: 2,
      rgbCorroboration: true,
    })).toBe('S74 · M52 · V5/16 · F3 · SIZE+35 · C2/4 · E1 · RGB');
  });

  it('does not pretend early view progress is 360-degree object coverage before a center exists', () => {
    const value = compactScanReadinessLabel(readiness({
      objectMajorExtentMeters: null,
      objectMinorExtentMeters: null,
      objectTooSmall: false,
      viewCoverage: 0.37,
      viewSectors: 0,
    }), {
      visibleCorners: 0,
      certifiedCorners: 0,
      certifiedEdges: 0,
      rgbCorroboration: false,
    });
    expect(value).toBe('S74 · M52 · V37 · F0');
    expect(value).not.toContain('/16');
  });

  it('reports plane and worker-edge maturity without claiming a certified edge', () => {
    const base = {
      visibleCorners: 0,
      certifiedCorners: 0,
      certifiedEdges: 0,
      rgbCorroboration: false,
    };
    expect(compactScanReadinessLabel(readiness({ objectTooSmall: false }), {
      ...base,
      planarPatches: 2,
      workerEdges: 0,
    })).toContain('F1');
    expect(compactScanReadinessLabel(readiness({ objectTooSmall: false }), {
      ...base,
      planarPatches: 2,
      workerEdges: 1,
    })).toContain('F2');
  });

  it('tells the operator to move when viewpoint diversity is the structural limiter', () => {
    const value = compactScanReadinessLabel(readiness({
      structural: { progress: 0.63, ready: false, limitingMetric: 'views', growthNeededPercent: 59 },
      objectTooSmall: false,
    }), {
      visibleCorners: 0,
      certifiedCorners: 0,
      certifiedEdges: 0,
      rgbCorroboration: false,
    });
    expect(value).toBe('S63 · M52 · V5/16 · F0 · MOVE');
  });

  it('keeps unavailable mesh readiness explicit and omits empty candidate counters', () => {
    expect(compactScanReadinessLabel(readiness({ mesh: null, objectTooSmall: false }), {
      visibleCorners: 0,
      certifiedCorners: 0,
      certifiedEdges: 0,
      rgbCorroboration: false,
    })).toBe('S74 · M-- · V5/16 · F0');
  });
});
