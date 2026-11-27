import { describe, expect, it } from 'vitest';

import { tsdfDiagnosticEvents, type TsdfDiagnosticsResult } from '../../src/xr/tsdfDiagnostics';

/* The client log keeps 420 characters per entry (src/data/clientDiagnostics.ts); the single SCAN_TSDF_READY of the 2026-11-26 run was cut mid-metrics. */
const LOG_ENTRY_LIMIT = 420;

function result(overrides: Partial<TsdfDiagnosticsResult> = {}): TsdfDiagnosticsResult {
  return {
    sourcePointCount: 3918, integratedPointCount: 2474, coarsenings: 0, channelTriangleCounts: [2189, 0, 1147, 3750, 21445, 0], meshRefine: { holesFilled: 6, smoothedVertices: 12399 },
    nodeCount: 23807, rawTriangleCount: 28279, triangleCount: 27075, quarantinedTriangleCount: 32, localNoiseHotspotCount: 4, localNoiseHotspotTriangleCount: 32,
    topologyDiscardedTriangleCount: 0, nonManifoldEdgeCount: 0, meshTopologyComponentCount: 11, orientationConflictCount: 0, selfIntersectionPairCount: 0,
    selfIntersectionExactTestedPairs: 123456, selfIntersectionBroadPhaseCandidatePairs: 654321, selfIntersectionTruncated: false, meshNoiseScore: 0.123456,
    meshReadiness: 1, voxelSizeMeters: 0.0576, truncationMeters: 0.2304, computeMs: 4623.456, directionalFallbackPoints: 1444,
    finalTopology: { triangles: 27075, components: 11, degenerateTriangles: 0, boundaryEdgeLengthMeters: 187.654321, nonManifoldEdges: 0 },
    ...overrides,
  };
}
const context = { scan: 'a1b2c3d4-0123456789abcdef-baseline-extra-long', certifiedPoints: 3918, roundTripMs: 4999.987, settings: 'optional-ai' };
const line = (event: string, fields: object): string => `${event} | ${JSON.stringify(fields)}`;

describe('TSDF diagnostics: bounded events that the client log does not cut (TASK 11)', () => {
  it('every event fits the log entry limit, even with the largest numbers the pipeline produces', () => {
    const worst = result({
      sourcePointCount: 9_999_999, integratedPointCount: 9_999_999, channelTriangleCounts: [99999, 99999, 99999, 99999, 99999, 99999], nodeCount: 99_999_999,
      rawTriangleCount: 9_999_999, triangleCount: 9_999_999, selfIntersectionExactTestedPairs: 999_999_999, selfIntersectionBroadPhaseCandidatePairs: 999_999_999,
      meshRefine: { holesFilled: 999999, smoothedVertices: 99999999 }, computeMs: 123456.789, directionalFallbackPoints: 9_999_999,
      finalTopology: { triangles: 9_999_999, components: 99999, degenerateTriangles: 99999, boundaryEdgeLengthMeters: 99999.999, nonManifoldEdges: 9999 },
    });
    for (const events of [tsdfDiagnosticEvents(result(), context), tsdfDiagnosticEvents(worst, { ...context, roundTripMs: 123456.789 })]) {
      for (const { event, fields } of events) expect(line(event, fields).length, event).toBeLessThanOrEqual(LOG_ENTRY_LIMIT - 40);
    }
  });

  it('the old single record WAS cut: the same fields in one entry exceed the limit (the defect, reproduced)', () => {
    const all = Object.assign({}, ...tsdfDiagnosticEvents(result(), context).map((e) => e.fields));
    expect(line('SCAN_TSDF_READY', all).length).toBeGreaterThan(LOG_ENTRY_LIMIT);
  });

  it('keeps the existing event name and carries the real metrics of the run, with no field lost', () => {
    const events = tsdfDiagnosticEvents(result(), context);
    expect(events.map((e) => e.event)).toEqual(['SCAN_TSDF_READY', 'SCAN_TSDF_COVERAGE', 'SCAN_TSDF_STITCH_STATS', 'SCAN_TSDF_FINAL_TOPOLOGY']);
    const merged = Object.assign({}, ...events.map((e) => e.fields));
    expect(merged).toMatchObject({
      triangles: 27075, rawTriangles: 28279, nodes: 23807, voxelMm: 58, truncationMm: 230, coarsenings: 0, certifiedPoints: 3918, sourcePoints: 3918, integratedPoints: 2474,
      directionalFallbackPoints: 1444, channelTriangles: '2189,0,1147,3750,21445,0', refineHolesFilled: 6, refineSmoothedVertices: 12399, quarantinedTriangles: 32,
      localHotspots: 4, components: 11, nonManifoldEdges: 0, selfIntersections: 0, selfIntersectionTruncated: false, degenerateTriangles: 0, boundaryEdgeM: 187.654, settings: 'optional-ai',
    });
  });

  it('every event names the scan so the pieces of one reconstruction can be joined, and a missing optional stage is null, not absent or invented', () => {
    const events = tsdfDiagnosticEvents(result({ directionalFallbackPoints: undefined, meshRefine: undefined, channelTriangleCounts: undefined, finalTopology: undefined }), context);
    for (const { fields } of events) expect(fields.scan).toBe(context.scan.slice(0, 24));
    const merged = Object.assign({}, ...events.map((e) => e.fields));
    expect(merged.directionalFallbackPoints).toBeNull();
    expect(merged.refineHolesFilled).toBeNull();
    expect(merged.channelTriangles).toBeNull();
    expect(merged.degenerateTriangles).toBeNull();
    expect(merged.boundaryEdgeM).toBeNull();
  });
});
