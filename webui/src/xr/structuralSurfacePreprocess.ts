import type { StructuralGuidePointCloudSnapshot } from './depthPointCloudV2';

export interface StructuralSurfaceEvidenceSnapshot extends StructuralGuidePointCloudSnapshot {
  residualMeters?: Float32Array;
  normalConfidence?: Float32Array;
  coverageConfidence?: Float32Array;
  coverageReady?: Uint8Array;
}

export const STRUCTURAL_MIN_CONFIDENCE = 0.58;
const STRUCTURAL_NORMAL_DOT = Math.cos((28 * Math.PI) / 180);
const MAX_LOCAL_CERTIFICATE_NEIGHBOURS = 18;

interface LocalCertificate {
  support: number;
  normalAgreement: number;
  residualMeters: number;
  confidence: number;
  ready: boolean;
}

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
}
function clamp01(value: number): number { return clamp(value, 0, 1); }
function coordinate(value: number, size: number): number { return Math.floor(value / size); }
function cellKey(x: number, y: number, z: number): string { return `${x}:${y}:${z}`; }

/**
 * Local physical-surface certificate used before structural geometry and TSDF.
 *
 * The input is already measured surfel geometry. This pass can reject weak/local
 * outliers and attach residual/normal/coverage diagnostics, but it never invents
 * positions and never consumes learned depth or native plane geometry. Keeping it
 * in a standalone pure module lets the live structural path execute it inside a
 * Dedicated Worker while manual placement can reuse exactly the same semantics.
 */
export function filterStructuralSurface(
  surface: StructuralGuidePointCloudSnapshot,
): StructuralSurfaceEvidenceSnapshot {
  const count = Math.min(
    surface.pointCount,
    Math.floor(surface.positions.length / 3),
    Math.floor(surface.normals.length / 3),
    surface.confidence.length,
  );
  if (count < 24) return surface;

  const cellSize = clamp(surface.voxelSizeMeters * 3.5, 0.070, 0.125);
  const radius = cellSize * 1.30;
  const radiusSquared = radius * radius;
  const buckets = new Map<string, number[]>();
  for (let index = 0; index < count; index += 1) {
    const offset = index * 3;
    const key = cellKey(
      coordinate(surface.positions[offset], cellSize),
      coordinate(surface.positions[offset + 1], cellSize),
      coordinate(surface.positions[offset + 2], cellSize),
    );
    const bucket = buckets.get(key) ?? [];
    bucket.push(index);
    buckets.set(key, bucket);
  }

  const accepted: number[] = [];
  const certificates = new Map<number, LocalCertificate>();
  for (let index = 0; index < count; index += 1) {
    if (surface.confidence[index] < STRUCTURAL_MIN_CONFIDENCE) continue;
    const offset = index * 3;
    const x = surface.positions[offset], y = surface.positions[offset + 1], z = surface.positions[offset + 2];
    const nx = surface.normals[offset], ny = surface.normals[offset + 1], nz = surface.normals[offset + 2];
    const normalLength = Math.hypot(nx, ny, nz);
    if (normalLength < 1e-6) continue;
    const nnx = nx / normalLength, nny = ny / normalLength, nnz = nz / normalLength;
    const gx = coordinate(x, cellSize), gy = coordinate(y, cellSize), gz = coordinate(z, cellSize);
    let compatibleSupport = 0, normalAgreementSum = 0, residualSum = 0;

    for (let dx = -1; dx <= 1 && compatibleSupport < MAX_LOCAL_CERTIFICATE_NEIGHBOURS; dx += 1) {
      for (let dy = -1; dy <= 1 && compatibleSupport < MAX_LOCAL_CERTIFICATE_NEIGHBOURS; dy += 1) {
        for (let dz = -1; dz <= 1 && compatibleSupport < MAX_LOCAL_CERTIFICATE_NEIGHBOURS; dz += 1) {
          const bucket = buckets.get(cellKey(gx + dx, gy + dy, gz + dz));
          if (bucket === undefined) continue;
          for (const candidate of bucket) {
            if (candidate === index) continue;
            const candidateOffset = candidate * 3;
            const px = surface.positions[candidateOffset] - x;
            const py = surface.positions[candidateOffset + 1] - y;
            const pz = surface.positions[candidateOffset + 2] - z;
            if (px * px + py * py + pz * pz > radiusSquared) continue;
            const cnx = surface.normals[candidateOffset], cny = surface.normals[candidateOffset + 1], cnz = surface.normals[candidateOffset + 2];
            const candidateNormalLength = Math.hypot(cnx, cny, cnz);
            if (candidateNormalLength < 1e-6) continue;
            const normalDot = Math.abs(
              nnx * cnx / candidateNormalLength + nny * cny / candidateNormalLength + nnz * cnz / candidateNormalLength
            );
            if (normalDot < STRUCTURAL_NORMAL_DOT) continue;
            compatibleSupport += 1;
            normalAgreementSum += normalDot;
            residualSum += Math.abs(nnx * px + nny * py + nnz * pz);
            if (compatibleSupport >= MAX_LOCAL_CERTIFICATE_NEIGHBOURS) break;
          }
        }
      }
    }

    if (compatibleSupport < 3) continue;
    const normalAgreement = normalAgreementSum / compatibleSupport;
    const residualMeters = residualSum / compatibleSupport;
    const supportScore = clamp01((compatibleSupport - 2) / 8);
    const residualScore = clamp01((0.035 - residualMeters) / 0.030);
    const normalScore = clamp01((normalAgreement - STRUCTURAL_NORMAL_DOT) / (1 - STRUCTURAL_NORMAL_DOT));
    const confidence = clamp01(
      surface.confidence[index] * 0.38 + supportScore * 0.24 + residualScore * 0.23 + normalScore * 0.15,
    );
    const ready = compatibleSupport >= 5 && residualMeters <= 0.030 && normalAgreement >= 0.86 && confidence >= 0.62;
    certificates.set(index, { support: compatibleSupport, normalAgreement, residualMeters, confidence, ready });
    if (ready) accepted.push(index);
  }

  // Fail open for measured geometry when this local certificate is too sparse to
  // be representative. Downstream robust fit/multi-view/FREE-space gates remain
  // responsible for rejecting geometry; this pass never turns absence of local
  // neighbours into proof that a physical surfel is false.
  if (accepted.length < Math.max(12, Math.floor(count * 0.22))) return surface;

  const positions = new Float32Array(accepted.length * 3);
  const normals = new Float32Array(accepted.length * 3);
  const confidence = new Float32Array(accepted.length);
  const residualMeters = new Float32Array(accepted.length);
  const normalConfidence = new Float32Array(accepted.length);
  const coverageConfidence = new Float32Array(accepted.length);
  const coverageReady = new Uint8Array(accepted.length);
  accepted.forEach((source, target) => {
    const sourceOffset = source * 3, targetOffset = target * 3;
    const certificate = certificates.get(source)!;
    positions[targetOffset] = surface.positions[sourceOffset];
    positions[targetOffset + 1] = surface.positions[sourceOffset + 1];
    positions[targetOffset + 2] = surface.positions[sourceOffset + 2];
    normals[targetOffset] = surface.normals[sourceOffset];
    normals[targetOffset + 1] = surface.normals[sourceOffset + 1];
    normals[targetOffset + 2] = surface.normals[sourceOffset + 2];
    confidence[target] = surface.confidence[source];
    residualMeters[target] = certificate.residualMeters;
    normalConfidence[target] = certificate.normalAgreement;
    coverageConfidence[target] = certificate.confidence;
    coverageReady[target] = certificate.ready ? 1 : 0;
  });
  return {
    positions,
    normals,
    confidence,
    residualMeters,
    normalConfidence,
    coverageConfidence,
    coverageReady,
    pointCount: accepted.length,
    keyframeCount: surface.keyframeCount,
    voxelSizeMeters: surface.voxelSizeMeters,
  };
}
