import type { ScanCapabilityReadiness } from './scanCapabilityReadiness';

export interface CompactGuideCounts {
  visibleCorners: number;
  certifiedCorners: number;
  certifiedEdges: number;
  rgbCorroboration: boolean;
}

function wholePercent(value: number): number {
  return Math.round(Math.max(0, Math.min(1, value)) * 100);
}

/**
 * Keep the live scanner bar short without overstating what has been measured.
 * Vn/16 means camera azimuth sectors around a measured component center, not
 * percentage of object surface. Before that center exists we show a plain view
 * progress percentage instead. SIZE+n is the additional linear scale implied by
 * the currently limiting measured extent, not an area percentage.
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

  if (readiness.objectTooSmall && readiness.structural.growthNeededPercent !== null) {
    parts.push(`SIZE+${readiness.structural.growthNeededPercent}`);
  } else if (!readiness.structural.ready && readiness.structural.limitingMetric === 'views') {
    // More frames from the same pose do not satisfy this gate. Tell the operator
    // to change viewpoint instead of silently encouraging more stationary scans.
    parts.push('MOVE');
  }
  if (counts.certifiedCorners > 0 || counts.certifiedEdges > 0) {
    parts.push(`C${counts.visibleCorners}/${counts.certifiedCorners}`);
    parts.push(`E${counts.certifiedEdges}`);
  }
  if (counts.rgbCorroboration) parts.push('RGB');
  return parts.join(' · ');
}
