import {
  DepthPointCloudAccumulator as SurfaceDepthPointCloudAccumulator,
  type CpuDepthInformationLike,
  type DepthFusionFrameContext,
  type DepthFusionResult,
  type DepthSampleView,
  type LiveCoverageSnapshot,
  type ProjectPoint,
  type ScanPointCloudSnapshot as SurfaceScanPointCloudSnapshot,
  type StructuralGuidePointCloudSnapshot,
} from './depthPointCloudV2';
import {
  registerManualPlacementResolver,
  solveManualPlacementOnStableSurface,
} from './manualPlacementSurface';
import {
  SparseOccupancyGrid,
  type OccupancySnapshot,
} from './occupancyEvidence';
import { recordActiveXrPerformanceMetric } from './xrPerformanceProfiler';

export type {
  ProjectPoint,
  PointCloudBounds,
  LiveCoverageSnapshot,
  DepthSampleView,
  CpuDepthInformationLike,
  DepthFusionFrameContext,
  DepthFusionResult,
} from './depthPointCloudV2';

export interface ScanPointCloudSnapshot
  extends SurfaceScanPointCloudSnapshot, OccupancySnapshot {
  /**
   * Final-mesh input is intentionally stricter than the display surfel cloud.
   * These arrays contain only locally certified multi-view surfels for which
   * residual, normal agreement and coverage readiness were all measured.
   */
  tsdfPositions: Float32Array;
  tsdfNormals: Float32Array;
  tsdfConfidence: Float32Array;
  tsdfResidualMeters: Float32Array;
  tsdfNormalConfidence: Float32Array;
  tsdfCoverageConfidence: Float32Array;
  tsdfCoverageReady: Uint8Array;
  tsdfPointCount: number;
}

interface StructuralSurfaceEvidenceSnapshot extends StructuralGuidePointCloudSnapshot {
  /** Local point-to-plane residual estimated from compatible promoted neighbours. */
  residualMeters?: Float32Array;
  /** Agreement of nearby promoted normals, 0..1. */
  normalConfidence?: Float32Array;
  /** Local geometric support score, distinct from global surfel confidence. */
  coverageConfidence?: Float32Array;
  /** 1 only when the local neighbourhood is dense and geometrically consistent. */
  coverageReady?: Uint8Array;
}

export interface StructuralGuideSnapshot extends StructuralSurfaceEvidenceSnapshot {
  /** Ray-verified free-space voxel centres used only to reject contradictory guide geometry. */
  freeSpacePositions: Float32Array;
  freeSpaceVoxelSizeMeters: number;
}

const STRUCTURAL_MIN_CONFIDENCE = 0.58;
const STRUCTURAL_NORMAL_DOT = Math.cos((28 * Math.PI) / 180);
const MAX_LOCAL_CERTIFICATE_NEIGHBOURS = 18;
const TSDF_MIN_CERTIFIED_POINTS = 60;

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
}

function clamp01(value: number): number {
  return clamp(value, 0, 1);
}

function coordinate(value: number, size: number): number {
  return Math.floor(value / size);
}

function cellKey(x: number, y: number, z: number): string {
  return `${x}:${y}:${z}`;
}

interface LocalCertificate {
  support: number;
  normalAgreement: number;
  residualMeters: number;
  confidence: number;
  ready: boolean;
}

/**
 * Structural inference is deliberately stricter than the display cloud.
 * Besides removing isolated promoted points, this computes a local geometric
 * certificate that downstream plane fitting and TSDF integration can use as
 * actual uncertainty evidence instead of treating all green surfels equally.
 */
function filterStructuralSurface(
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
    const x = surface.positions[offset];
    const y = surface.positions[offset + 1];
    const z = surface.positions[offset + 2];
    const nx = surface.normals[offset];
    const ny = surface.normals[offset + 1];
    const nz = surface.normals[offset + 2];
    const normalLength = Math.hypot(nx, ny, nz);
    if (normalLength < 1e-6) continue;
    const invNormalLength = 1 / normalLength;
    const nnx = nx * invNormalLength;
    const nny = ny * invNormalLength;
    const nnz = nz * invNormalLength;
    const gx = coordinate(x, cellSize);
    const gy = coordinate(y, cellSize);
    const gz = coordinate(z, cellSize);
    let compatibleSupport = 0;
    let normalAgreementSum = 0;
    let residualSum = 0;

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
            const cnx = surface.normals[candidateOffset];
            const cny = surface.normals[candidateOffset + 1];
            const cnz = surface.normals[candidateOffset + 2];
            const candidateNormalLength = Math.hypot(cnx, cny, cnz);
            if (candidateNormalLength < 1e-6) continue;
            const normalDot = Math.abs(
              nnx * cnx / candidateNormalLength
              + nny * cny / candidateNormalLength
              + nnz * cnz / candidateNormalLength
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
      surface.confidence[index] * 0.38 +
      supportScore * 0.24 +
      residualScore * 0.23 +
      normalScore * 0.15,
    );
    const ready = compatibleSupport >= 5 && residualMeters <= 0.030 && normalAgreement >= 0.86 && confidence >= 0.62;
    certificates.set(index, { support: compatibleSupport, normalAgreement, residualMeters, confidence, ready });
    if (ready) accepted.push(index);
  }

  // An early scan may not yet have enough certified cells. Keep the original
  // surface for live guide diagnostics, but omit TSDF evidence until real local
  // certificates exist; this is preferable to rendering a noisy fake surface.
  if (accepted.length < Math.max(12, Math.floor(count * 0.22))) return surface;

  const positions = new Float32Array(accepted.length * 3);
  const normals = new Float32Array(accepted.length * 3);
  const confidence = new Float32Array(accepted.length);
  const residualMeters = new Float32Array(accepted.length);
  const normalConfidence = new Float32Array(accepted.length);
  const coverageConfidence = new Float32Array(accepted.length);
  const coverageReady = new Uint8Array(accepted.length);
  accepted.forEach((source, target) => {
    const sourceOffset = source * 3;
    const targetOffset = target * 3;
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

export class DepthPointCloudAccumulator extends SurfaceDepthPointCloudAccumulator {
  private readonly occupancy = new SparseOccupancyGrid();

  constructor(
    voxelSizeMeters?: number,
    memoryBudgetPoints?: number,
    maxDepthMeters?: number,
  ) {
    super(voxelSizeMeters, memoryBudgetPoints, maxDepthMeters);
    registerManualPlacementResolver((candidate: ProjectPoint) =>
      solveManualPlacementOnStableSurface(candidate, this.structuralGuideSnapshot())
    );
  }

  override reset(): void {
    super.reset();
    this.occupancy.reset();
  }

  override sampleDepthFrame(
    depth: CpuDepthInformationLike,
    fallbackView: DepthSampleView,
    context: DepthFusionFrameContext,
  ): DepthFusionResult {
    const result = super.sampleDepthFrame(depth, fallbackView, context);
    this.occupancy.integrateDepthFrame(depth, fallbackView, {
      ...context,
      maxDepthMeters: this.maxDepth,
    });
    return result;
  }

  override liveCoverageSnapshot(): LiveCoverageSnapshot {
    const started = performance.now();
    try {
      return super.liveCoverageSnapshot();
    } finally {
      recordActiveXrPerformanceMetric('coverageSnapshot', performance.now() - started);
    }
  }

  override structuralGuideSnapshot(): StructuralGuideSnapshot {
    const surface = filterStructuralSurface(super.structuralGuideSnapshot());
    const occupancy = this.occupancy.snapshot(null);
    return {
      ...surface,
      freeSpacePositions: occupancy.freeAirPositions,
      freeSpaceVoxelSizeMeters: occupancy.occupancyVoxelSizeMeters,
    };
  }

  override snapshot(
    depthFrameCount: number,
    sampledDepthFrames: number,
  ): ScanPointCloudSnapshot {
    const surface = super.snapshot(depthFrameCount, sampledDepthFrames);
    const occupancy = this.occupancy.snapshot(surface.bounds);
    const certified = filterStructuralSurface(super.structuralGuideSnapshot());
    const hasTsdfCertificate =
      certified.pointCount >= TSDF_MIN_CERTIFIED_POINTS &&
      certified.residualMeters !== undefined &&
      certified.normalConfidence !== undefined &&
      certified.coverageConfidence !== undefined &&
      certified.coverageReady !== undefined;

    return {
      ...surface,
      ...occupancy,
      tsdfPositions: hasTsdfCertificate ? certified.positions : new Float32Array(0),
      tsdfNormals: hasTsdfCertificate ? certified.normals : new Float32Array(0),
      tsdfConfidence: hasTsdfCertificate ? certified.confidence : new Float32Array(0),
      tsdfResidualMeters: hasTsdfCertificate ? certified.residualMeters! : new Float32Array(0),
      tsdfNormalConfidence: hasTsdfCertificate ? certified.normalConfidence! : new Float32Array(0),
      tsdfCoverageConfidence: hasTsdfCertificate ? certified.coverageConfidence! : new Float32Array(0),
      tsdfCoverageReady: hasTsdfCertificate ? certified.coverageReady! : new Uint8Array(0),
      tsdfPointCount: hasTsdfCertificate ? certified.pointCount : 0,
    };
  }
}
