import type { SparseTsdfMesh } from './sparseTsdf';

export interface MeshQualityUiStatus {
  label: string;
  severity: 'ok' | 'warning' | 'bad';
  rescanRecommended: boolean;
}

const NOISY_MESH_SCORE = 0.60;

/**
 * Compact post-scan mesh status. The status never changes geometry; it only
 * exposes already-computed topology/noise diagnostics to the operator.
 */
export function describeMeshQuality(mesh: SparseTsdfMesh): MeshQualityUiStatus {
  const noisePercent = Math.round(Math.max(0, Math.min(1, mesh.meshNoiseScore)) * 100);
  if (mesh.selfIntersectionTruncated) {
    return {
      label: `MESH CHECK LIMITED · ${noisePercent}% noise · RESCAN`,
      severity: 'bad',
      rescanRecommended: true,
    };
  }
  if (mesh.meshNoiseScore > NOISY_MESH_SCORE || mesh.quarantinedTriangleCount > 0) {
    return {
      label: `MESH NOISY ${noisePercent}% · Q${mesh.quarantinedTriangleCount.toLocaleString()} · RESCAN`,
      severity: 'bad',
      rescanRecommended: true,
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
