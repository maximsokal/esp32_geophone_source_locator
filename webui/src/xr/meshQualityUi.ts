import type { SparseTsdfMesh } from './sparseTsdf';

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
export function describeMeshQuality(mesh: SparseTsdfMesh): MeshQualityUiStatus {
  const readinessPercent = Math.round(Math.max(0, Math.min(1, mesh.meshReadiness)) * 100);
  const noisePercent = Math.round(Math.max(0, Math.min(1, mesh.meshNoiseScore)) * 100);
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
      label: `MESH ${readinessPercent}% · SCAN WIDER`,
      severity: 'warning',
      rescanRecommended: false,
    };
  }
  if (mesh.selfIntersectionTruncated) {
    return {
      label: mesh.triangleCount > 0
        ? `MESH PARTIAL · ${mesh.triangleCount.toLocaleString()}△ · CHECK LIMITED · ${noisePercent}% noise`
        : `MESH CHECK LIMITED · ${noisePercent}% noise · RESCAN`,
      severity: 'bad',
      rescanRecommended: true,
    };
  }
  if (mesh.meshNoiseScore > NOISY_MESH_SCORE || componentQuarantinedTriangles > 0) {
    return {
      label: mesh.triangleCount > 0
        ? `MESH PARTIAL · ${mesh.triangleCount.toLocaleString()}△ · ${noisePercent}% noise · Q${componentQuarantinedTriangles.toLocaleString()}`
        : `MESH NOISY ${noisePercent}% · Q${componentQuarantinedTriangles.toLocaleString()} · RESCAN`,
      severity: 'bad',
      rescanRecommended: true,
    };
  }
  if (mesh.triangleCount === 0) {
    return {
      label: 'MESH EMPTY · RESCAN',
      severity: 'bad',
      rescanRecommended: true,
    };
  }
  if (mesh.localNoiseHotspotTriangleCount > 0) {
    return {
      label: `MESH REPAIRED · H${mesh.localNoiseHotspotCount.toLocaleString()} · ${mesh.triangleCount.toLocaleString()}△`,
      severity: 'warning',
      rescanRecommended: false,
    };
  }
  if (mesh.selfIntersectionPairCount > 0 || mesh.orientationConflictCount > 0 || mesh.nonManifoldEdgeCount > 0) {
    return {
      label: `MESH CHECK ${noisePercent}% · ${mesh.triangleCount.toLocaleString()}△`,
      severity: 'warning',
      rescanRecommended: false,
    };
  }
  return {
    label: `MESH OK · ${mesh.triangleCount.toLocaleString()}△`,
    severity: 'ok',
    rescanRecommended: false,
  };
}
