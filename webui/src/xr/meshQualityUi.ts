import type { SparseTsdfMesh } from './sparseTsdf';
import { translate, type I18nLocale } from '../i18n';
import { formatNumber, formatPercent } from '../i18n/format';

export interface MeshQualityUiStatus {
  label: string;
  severity: 'ok' | 'warning' | 'bad';
  rescanRecommended: boolean;
}

const NOISY_MESH_SCORE = 0.60;

/**
 * Compact post-scan mesh status. The status never changes geometry; it only
 * exposes already-computed readiness/topology/noise diagnostics to the operator.
 *
 * A small correlated hotspot that the bounded sanitizer successfully removed is
 * not equivalent to losing an entire mesh component. Keep it visible as a
 * warning, but do not force a full rescan unless component quarantine, diagnostic
 * truncation or a high global noise score remains after the repair.
 */
export function describeMeshQuality(mesh: SparseTsdfMesh, locale: I18nLocale = 'en'): MeshQualityUiStatus {
  const percent = (value: number): string => formatPercent(locale, Math.max(0, Math.min(1, value)));
  const readinessPercent = percent(mesh.meshReadiness);
  const noisePercent = percent(mesh.meshNoiseScore);
  const triangles = formatNumber(locale, mesh.triangleCount);
  const localHotspotTriangles = Math.max(
    0,
    Math.min(mesh.quarantinedTriangleCount, mesh.localNoiseHotspotTriangleCount),
  );
  const componentQuarantinedTriangles = Math.max(
    0,
    mesh.quarantinedTriangleCount - localHotspotTriangles,
  );

  if (mesh.meshReadiness < 1 - 1e-9) {
    return {
      label: translate(locale, 'xr.mesh.scan_wider', { percent: readinessPercent }),
      severity: 'warning',
      rescanRecommended: false,
    };
  }
  if (mesh.selfIntersectionTruncated) {
    return {
      label: mesh.triangleCount > 0
        ? translate(locale, 'xr.mesh.partial_limited', { count: triangles, noise: noisePercent })
        : translate(locale, 'xr.mesh.check_limited_rescan', { noise: noisePercent }),
      severity: 'bad',
      rescanRecommended: true,
    };
  }
  if (mesh.meshNoiseScore > NOISY_MESH_SCORE || componentQuarantinedTriangles > 0) {
    return {
      label: mesh.triangleCount > 0
        ? translate(locale, 'xr.mesh.partial_noisy', { count: triangles, noise: noisePercent, quarantined: formatNumber(locale, componentQuarantinedTriangles) })
        : translate(locale, 'xr.mesh.noisy_rescan', { noise: noisePercent, quarantined: formatNumber(locale, componentQuarantinedTriangles) }),
      severity: 'bad',
      rescanRecommended: true,
    };
  }
  if (mesh.triangleCount === 0) {
    return {
      label: translate(locale, 'xr.mesh.empty_rescan'),
      severity: 'bad',
      rescanRecommended: true,
    };
  }
  /* The sanitizer removed correlated noise spots; that says nothing about gaps in the surface, so the label names what was done (cleaned), not "repaired". */
  if (mesh.localNoiseHotspotTriangleCount > 0) {
    return {
      label: translate(locale, 'xr.mesh.cleaned', { hotspots: formatNumber(locale, mesh.localNoiseHotspotCount), count: triangles }),
      severity: 'warning',
      rescanRecommended: false,
    };
  }
  if (mesh.selfIntersectionPairCount > 0 || mesh.orientationConflictCount > 0 || mesh.nonManifoldEdgeCount > 0) {
    return {
      label: translate(locale, 'xr.mesh.check', { noise: noisePercent, count: triangles }),
      severity: 'warning',
      rescanRecommended: false,
    };
  }
  return {
    label: translate(locale, 'xr.mesh.ok', { count: triangles }),
    severity: 'ok',
    rescanRecommended: false,
  };
}
