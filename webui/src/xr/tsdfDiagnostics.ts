/**
 * The diagnostics of one post-scan TSDF reconstruction, as SEVERAL bounded events instead of one long record. The client log keeps at most 420 characters
 * per entry, and the single SCAN_TSDF_READY of the 2026-11-26 run was cut in the middle of its metrics (TASK 11). Every event below stays well under that
 * limit, carries the scan identity (`scan`) so the pieces can be joined, and holds only what its name says. Pure; the viewport only logs what this returns.
 */
export interface TsdfDiagnosticsResult {
  sourcePointCount: number;
  integratedPointCount: number;
  coarsenings?: number;
  channelTriangleCounts?: number[];
  meshRefine?: { holesFilled: number; smoothedVertices: number };
  nodeCount: number;
  rawTriangleCount: number;
  triangleCount: number;
  quarantinedTriangleCount: number;
  localNoiseHotspotCount: number;
  localNoiseHotspotTriangleCount: number;
  topologyDiscardedTriangleCount: number;
  nonManifoldEdgeCount: number;
  meshTopologyComponentCount: number;
  orientationConflictCount: number;
  selfIntersectionPairCount: number;
  selfIntersectionExactTestedPairs: number;
  selfIntersectionBroadPhaseCandidatePairs: number;
  selfIntersectionTruncated: boolean;
  meshNoiseScore: number;
  meshReadiness: number;
  voxelSizeMeters: number;
  truncationMeters: number;
  computeMs: number;
  directionalFallbackPoints?: number;
  finalTopology?: { triangles: number; components: number; degenerateTriangles: number; boundaryEdgeLengthMeters: number; nonManifoldEdges?: number } | undefined;
}

export interface TsdfDiagnosticsContext {
  /** Identity of the scan the mesh was built for (frame id / fingerprint): lets the events of one reconstruction be joined. */
  scan: string;
  certifiedPoints: number;
  roundTripMs: number;
  settings: string;
}

export interface TsdfDiagnosticEvent { event: string; fields: Record<string, string | number | boolean | null> }

const r1 = (value: number): number => Number(value.toFixed(1));
const r3 = (value: number): number => Number(value.toFixed(3));

export function tsdfDiagnosticEvents(result: TsdfDiagnosticsResult, context: TsdfDiagnosticsContext): TsdfDiagnosticEvent[] {
  const scan = context.scan.slice(0, 24);
  return [
    { event: 'SCAN_TSDF_READY', fields: {
      scan, settings: context.settings, triangles: result.triangleCount, rawTriangles: result.rawTriangleCount, nodes: result.nodeCount,
      voxelMm: Math.round(result.voxelSizeMeters * 1000), truncationMm: Math.round(result.truncationMeters * 1000), coarsenings: result.coarsenings ?? 0,
      meshReadiness: r3(result.meshReadiness), meshNoiseScore: r3(result.meshNoiseScore), computeMs: r1(result.computeMs), roundTripMs: r1(context.roundTripMs),
    } },
    { event: 'SCAN_TSDF_COVERAGE', fields: {
      scan, certifiedPoints: context.certifiedPoints, sourcePoints: result.sourcePointCount, integratedPoints: result.integratedPointCount,
      directionalFallbackPoints: result.directionalFallbackPoints ?? null,
    } },
    { event: 'SCAN_TSDF_STITCH_STATS', fields: {
      scan, channelTriangles: result.channelTriangleCounts?.join(',') ?? null, refineHolesFilled: result.meshRefine?.holesFilled ?? null,
      refineSmoothedVertices: result.meshRefine?.smoothedVertices ?? null, quarantinedTriangles: result.quarantinedTriangleCount,
      localHotspots: result.localNoiseHotspotCount, localHotspotTriangles: result.localNoiseHotspotTriangleCount, discardedTriangles: result.topologyDiscardedTriangleCount,
    } },
    { event: 'SCAN_TSDF_FINAL_TOPOLOGY', fields: {
      scan, components: result.meshTopologyComponentCount, nonManifoldEdges: result.nonManifoldEdgeCount, orientationConflicts: result.orientationConflictCount,
      selfIntersections: result.selfIntersectionPairCount, selfIntersectionTruncated: result.selfIntersectionTruncated,
      selfIntersectionExactTests: result.selfIntersectionExactTestedPairs, selfIntersectionBroadPairs: result.selfIntersectionBroadPhaseCandidatePairs,
      degenerateTriangles: result.finalTopology?.degenerateTriangles ?? null, boundaryEdgeM: result.finalTopology === undefined ? null : r3(result.finalTopology.boundaryEdgeLengthMeters),
    } },
  ];
}
