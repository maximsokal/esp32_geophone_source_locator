import type { ProjectPoint } from './structuralGuide';

export interface StructuralEdgeScaleEvidence {
  start: ProjectPoint;
  end: ProjectPoint;
  support: number;
}

// A useful metric edge should span more than roughly two nominal 4-cm source
// voxels. Local plane-patch fragments may be shorter; they are joined before the
// operational scale gate is applied.
export const MIN_CERTIFIED_EDGE_LENGTH_METERS = 0.09;
export const MIN_CERTIFIED_EDGE_SUPPORT = 6;
export const EDGE_SUPPORT_INTERVAL_METERS = 0.075;
export const MIN_EDGE_SUPPORT_DENSITY_PER_METER = 22;

export function structuralEdgeLengthMeters(edge: Pick<StructuralEdgeScaleEvidence, 'start' | 'end'>): number {
  return Math.hypot(
    edge.end[0] - edge.start[0],
    edge.end[1] - edge.start[1],
    edge.end[2] - edge.start[2],
  );
}

/**
 * Scale-aware physical support gate shared by line certification and any
 * downstream vertex/corner inference. Long lines must carry proportionally more
 * measured support; a handful of points can never justify a metre-long edge.
 */
export function isScaleSupportedStructuralEdge(edge: StructuralEdgeScaleEvidence): boolean {
  const length = structuralEdgeLengthMeters(edge);
  if (!Number.isFinite(length) || length < MIN_CERTIFIED_EDGE_LENGTH_METERS) return false;
  const requiredSupport = Math.max(
    MIN_CERTIFIED_EDGE_SUPPORT,
    Math.ceil(length / EDGE_SUPPORT_INTERVAL_METERS) * 2,
  );
  const support = Number(edge.support);
  const supportDensity = support / Math.max(length, MIN_CERTIFIED_EDGE_LENGTH_METERS);
  return Number.isFinite(support) && support >= requiredSupport && supportDensity >= MIN_EDGE_SUPPORT_DENSITY_PER_METER;
}
