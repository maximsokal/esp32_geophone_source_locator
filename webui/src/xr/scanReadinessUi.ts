import type { ScanCapabilityReadiness } from './scanCapabilityReadiness';

export interface CompactGuideCounts {
  visibleCorners: number;
  certifiedCorners: number;
  certifiedEdges: number;
  /** Robust plane patches produced by the current physical structural solve. */
  planarPatches?: number;
  /** Finite-support worker edges before final scale/visual certification. */
  workerEdges?: number;
  rgbCorroboration: boolean;
}

export type StructuralFeatureStage = 0 | 1 | 2 | 3;

function wholePercent(value: number): number {
  return Math.round(Math.max(0, Math.min(1, value)) * 100);
}

/**
 * Compact feature maturity is intentionally a stage, not a fake percentage:
 * F0 no two-plane evidence yet; F1 >=2 robust physical plane patches exist;
 * F2 a finite locally-supported worker edge is being tracked; F3 at least one
 * edge passed final metric scale/support certification. The stage never creates
 * geometry and does not claim a corner exists merely because enough points were
 * collected.
 */
export function structuralFeatureStage(counts: CompactGuideCounts): StructuralFeatureStage {
  if (counts.certifiedEdges > 0) return 3;
  if ((counts.workerEdges ?? 0) > 0) return 2;
  if ((counts.planarPatches ?? 0) >= 2) return 1;
  return 0;
}

/**
 * Keep the live scanner bar short without overstating what has been measured.
 * Vn/16 means camera azimuth sectors around a measured component center, not
 * percentage of object surface. Before that center exists we show a plain view
 * progress percentage instead. SIZE+n is the additional linear scale implied by
 * the currently limiting measured extent, not an area percentage. F0..F3 reports
 * actual solver feature maturity and therefore remains meaningful even at S100.
 */
export function compactScanReadinessLabel(
  readiness: ScanCapabilityReadiness,
  counts: CompactGuideCounts,
): string {
  const parts = [`S${wholePercent(readiness.structural.progress)}`];
  parts.push(readiness.mesh === null ? 'M--' : `M${wholePercent(readiness.mesh.progress)}`);

  const hasMeasuredCenter = readiness.objectMajorExtentMeters !== null && readiness.objectMinorExtentMeters !== null;
  parts.push(hasMeasuredCenter
    ? `V${readiness.viewSectors}/${readiness.totalViewSectors}`
    : `V${wholePercent(readiness.viewCoverage)}`);
  parts.push(`F${structuralFeatureStage(counts)}`);

  if (readiness.objectTooSmall && readiness.structural.growthNeededPercent !== null) {
    parts.push(`SIZE+${readiness.structural.growthNeededPercent}`);
  } else if (!readiness.structural.ready && readiness.structural.limitingMetric === 'views') {
    // More frames from the same pose do not satisfy this gate. Tell the operator
    // to change viewpoint instead of silently encouraging more stationary scans.
    parts.push('MOVE');
  } else if (!readiness.structural.ready && readiness.structural.limitingMetric === 'faces') {
    // MAX-254: one face is measured well, but an edge needs an adjacent,
    // non-parallel face: scan around the corner rather than the same side.
    parts.push('FACE+');
  }
  if (counts.certifiedCorners > 0 || counts.certifiedEdges > 0) {
    parts.push(`C${counts.visibleCorners}/${counts.certifiedCorners}`);
    parts.push(`E${counts.certifiedEdges}`);
  }
  if (counts.rgbCorroboration) parts.push('RGB');
  return parts.join(' · ');
}
