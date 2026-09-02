import { describe, expect, it } from 'vitest';

import { describeMeshQuality } from '../../src/xr/meshQualityUi';
import type { SparseTsdfMesh } from '../../src/xr/sparseTsdf';

function mesh(overrides: Partial<SparseTsdfMesh> = {}): SparseTsdfMesh {
  return {
    trianglePositions: new Float32Array(0),
    triangleCount: 120,
    rawTriangleCount: 120,
    quarantinedTriangleCount: 0,
    topologyDiscardedTriangleCount: 0,
    nonManifoldEdgeCount: 0,
    nonManifoldRepairTriangleCount: 0,
    orientationRepairCount: 0,
    orientationConflictCount: 0,
    meshTopologyComponentCount: 1,
    selfIntersectionPairCount: 0,
    selfIntersectionAffectedTriangleCount: 0,
    selfIntersectionExactTestedPairs: 0,
    selfIntersectionBroadPhaseCandidatePairs: 0,
    selfIntersectionTruncated: false,
    selfIntersectionOversizedTriangleCount: 0,
    meshNoiseScore: 0,
    sourcePointCount: 100,
    integratedPointCount: 100,
    skippedLowConfidence: 0,
    rejectedComponentPoints: 0,
    componentCount: 1,
    eligibleComponentCount: 1,
    meshReadiness: 1,
    nodeCount: 80,
    voxelSizeMeters: 0.048,
    truncationMeters: 0.132,
    computeMs: 10,
    ...overrides,
  };
}

describe('mesh quality UI status', () => {
  it('keeps a clean mesh status compact', () => {
    const result = describeMeshQuality(mesh());
    expect(result).toEqual({ label: 'MESH OK · 120△', severity: 'ok', rescanRecommended: false });
  });

  it('asks for a wider scan when reconstruction preflight deferred the mesh', () => {
    const result = describeMeshQuality(mesh({ meshReadiness: 0.64, triangleCount: 0, rawTriangleCount: 0 }));
    expect(result).toEqual({ label: 'MESH 64% · SCAN WIDER', severity: 'warning', rescanRecommended: false });
  });

  it('recommends rescan when a mature component was quarantined', () => {
    const result = describeMeshQuality(mesh({ meshNoiseScore: 0.73, quarantinedTriangleCount: 24 }));
    expect(result.severity).toBe('bad');
    expect(result.rescanRecommended).toBe(true);
    expect(result.label).toContain('MESH NOISY 73%');
    expect(result.label).toContain('Q24');
  });

  it('treats a bounded diagnostic truncation as incomplete rather than clean', () => {
    const result = describeMeshQuality(mesh({ selfIntersectionTruncated: true, meshNoiseScore: 0.12 }));
    expect(result.severity).toBe('bad');
    expect(result.rescanRecommended).toBe(true);
    expect(result.label).toContain('CHECK LIMITED');
  });

  it('recommends rescan when a ready reconstruction unexpectedly produces no retained triangles', () => {
    const result = describeMeshQuality(mesh({ triangleCount: 0, rawTriangleCount: 12 }));
    expect(result).toEqual({ label: 'MESH EMPTY · RESCAN', severity: 'bad', rescanRecommended: true });
  });

  it('shows a warning without forcing rescan for isolated low-score topology evidence', () => {
    const result = describeMeshQuality(mesh({ selfIntersectionPairCount: 2, meshNoiseScore: 0.18 }));
    expect(result.severity).toBe('warning');
    expect(result.rescanRecommended).toBe(false);
    expect(result.label).toContain('MESH CHECK 18%');
  });
});
