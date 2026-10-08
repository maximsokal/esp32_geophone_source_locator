import type { CpuDepthInformationLike } from './depthPointCloudV2';
import type { ProjectPoint } from './manualControlPoints';
import {
  calibrateMonocularDepthPriorDetailed,
  calibratedPriorDepthMeters,
  DepthAnythingV2WebGpuProvider,
  type LogAffineDepthCalibration,
  type MetricDepthAnchor,
  type MonocularDepthCalibrationDiagnostics,
  type MonocularDepthPriorFrame,
  type MonocularDepthProvider,
  type MonocularDepthProviderStatus,
} from './monocularDepthPrior';
import {
  multiplyMat4,
  projectProjectPointToNormalized,
  rigidInverse,
} from './projectionMath';
import {
  rgbGuidanceMatchesView,
  sampleRgbGuidance,
  type RgbDepthGuidanceFrame,
} from './rgbDepthGuidance';

export interface PhysicalDepthGuardFrame {
  width: number;
  height: number;
  depthMeters: Float32Array;
}

interface CalibratedPrior {
  frame: MonocularDepthPriorFrame;
  calibration: LogAffineDepthCalibration;
  /** RGB frame/pose whose pixels produced this monocular depth map. */
  captureGuidance: RgbDepthGuidanceFrame;
  /** Sparse physical frontmost depth from exactly the same staged XR view. */
  physicalGuard: PhysicalDepthGuardFrame;
  /** Cached once per calibrated frame; bulk filtering must not rebuild matrices per surfel. */
  viewFromWorld: Float32Array | null;
  viewProjection: Float32Array | null;
}

interface StagedPostScanPrior {
  guidance: RgbDepthGuidanceFrame;
  anchors: MetricDepthAnchor[];
  physicalGuard: PhysicalDepthGuardFrame;
}

export interface MonocularSurfaceAgreement {
  available: boolean;
  accepted: boolean;
  priorMeters: number | null;
  physicalMeters: number;
  disagreementMeters: number | null;
  calibrationConfidence: number;
}

const MAX_PRIOR_AGE_MS = 9000;
const MIN_CALIBRATION_CONFIDENCE = 0.66;
const MIN_CALIBRATION_INLIERS = 10;
const ANCHOR_COLUMNS = 8;
const ANCHOR_ROWS = 6;
const GUARD_COLUMNS = 16;
const GUARD_ROWS = 9;
const GUARD_EDGE_STRENGTH = 0.20;
const GUARD_ABSOLUTE_TOLERANCE_METERS = 0.12;
const GUARD_RELATIVE_TOLERANCE = 0.12;

function validPhysicalDepth(value: number): boolean {
  return Number.isFinite(value) && value >= 0.18 && value <= 8;
}

function cloneGuidance(frame: RgbDepthGuidanceFrame): RgbDepthGuidanceFrame {
  return {
    ...frame,
    luma: frame.luma.slice(),
    rgba: frame.rgba?.slice(),
    edgeStrength: frame.edgeStrength.slice(),
    gradientX: frame.gradientX.slice(),
    gradientY: frame.gradientY.slice(),
    projectionMatrix: frame.projectionMatrix?.slice(),
    worldFromView: frame.worldFromView?.slice(),
  };
}

function guidanceAsRgba(frame: RgbDepthGuidanceFrame): Uint8Array {
  const expected = frame.width * frame.height * 4;
  if (frame.rgba !== undefined && frame.rgba.length >= expected) {
    // stagePostScan already owns this cloned packet; avoid a second full-frame copy.
    return frame.rgba.length === expected ? frame.rgba : frame.rgba.subarray(0, expected);
  }
  const rgba = new Uint8Array(expected);
  for (let index = 0; index < frame.luma.length; index += 1) {
    const value = frame.luma[index];
    const offset = index * 4;
    rgba[offset] = value;
    rgba[offset + 1] = value;
    rgba[offset + 2] = value;
    rgba[offset + 3] = 255;
  }
  return rgba;
}

export function collectPhysicalMetricAnchors(
  depth: CpuDepthInformationLike,
  guidance: RgbDepthGuidanceFrame,
): MetricDepthAnchor[] {
  const anchors: MetricDepthAnchor[] = [];
  for (let row = 0; row < ANCHOR_ROWS; row += 1) {
    const v = (row + 0.5) / ANCHOR_ROWS;
    for (let column = 0; column < ANCHOR_COLUMNS; column += 1) {
      const u = (column + 0.5) / ANCHOR_COLUMNS;
      const visual = sampleRgbGuidance(guidance, u, v);
      if (visual !== null && visual.edgeStrength >= GUARD_EDGE_STRENGTH) continue;
      const metric = depth.getDepthInMeters(u, v);
      if (!validPhysicalDepth(metric)) continue;
      anchors.push({
        u,
        v,
        depthMeters: metric,
        weight: visual === null ? 0.65 : Math.max(0.35, 1 - visual.edgeStrength * 2.2),
      });
    }
  }
  return anchors;
}

/**
 * Small staged physical-depth grid from the exact RGB capture pose. It is not
 * used to create geometry. Its sole purpose is to stop the monocular prior from
 * vetoing a real surfel that was hidden behind another surface in this view.
 */
export function collectPhysicalDepthGuard(
  depth: CpuDepthInformationLike,
  guidance: RgbDepthGuidanceFrame,
): PhysicalDepthGuardFrame {
  const depthMeters = new Float32Array(GUARD_COLUMNS * GUARD_ROWS);
  depthMeters.fill(Number.NaN);
  for (let row = 0; row < GUARD_ROWS; row += 1) {
    const v = (row + 0.5) / GUARD_ROWS;
    for (let column = 0; column < GUARD_COLUMNS; column += 1) {
      const u = (column + 0.5) / GUARD_COLUMNS;
      const visual = sampleRgbGuidance(guidance, u, v);
      if (visual !== null && visual.edgeStrength >= GUARD_EDGE_STRENGTH) continue;
      const metric = depth.getDepthInMeters(u, v);
      if (!validPhysicalDepth(metric)) continue;
      depthMeters[row * GUARD_COLUMNS + column] = metric;
    }
  }
  return { width: GUARD_COLUMNS, height: GUARD_ROWS, depthMeters };
}

export function samplePhysicalDepthGuard(
  guard: PhysicalDepthGuardFrame,
  u: number,
  v: number,
): number | null {
  if (guard.width <= 0 || guard.height <= 0 || guard.depthMeters.length < guard.width * guard.height) return null;
  const x = Math.max(0, Math.min(guard.width - 1, Math.floor(Math.max(0, Math.min(1, u)) * guard.width)));
  const y = Math.max(0, Math.min(guard.height - 1, Math.floor(Math.max(0, Math.min(1, v)) * guard.height)));
  const metric = guard.depthMeters[y * guard.width + x];
  return validPhysicalDepth(metric) ? metric : null;
}

/** True only when the surfel is plausibly the same frontmost physical surface. */
export function physicalDepthGuardAllowsAiVeto(
  guard: PhysicalDepthGuardFrame,
  u: number,
  v: number,
  viewDepthMeters: number,
): boolean {
  if (!validPhysicalDepth(viewDepthMeters)) return false;
  const physicalFrontmost = samplePhysicalDepthGuard(guard, u, v);
  if (physicalFrontmost === null) return false;
  const tolerance = Math.max(
    GUARD_ABSOLUTE_TOLERANCE_METERS,
    physicalFrontmost * GUARD_RELATIVE_TOLERANCE,
  );
  return Math.abs(viewDepthMeters - physicalFrontmost) <= tolerance;
}

export class MonocularPriorRuntime {
  private enabled = false;
  private providerBusy = false;
  private providerBusyEpoch: number | null = null;
  private epoch = 1;
  private calibrated: CalibratedPrior | null = null;
  private staged: StagedPostScanPrior | null = null;
  private lastError: string | null = null;
  private lastCalibrationDiagnostics: MonocularDepthCalibrationDiagnostics | null = null;
  private readonly listeners = new Set<() => void>();

  constructor(private readonly provider: MonocularDepthProvider) {}

  subscribe(listener: () => void): () => void {
    this.listeners.add(listener);
    listener();
    return () => {
      this.listeners.delete(listener);
    };
  }

  private notify(): void {
    for (const listener of this.listeners) listener();
  }

  setEnabled(enabled: boolean): void {
    if (this.enabled === enabled) return;
    this.enabled = enabled;
    this.epoch += 1;
    this.providerBusy = false;
    this.providerBusyEpoch = null;
    this.calibrated = null;
    this.staged = null;
    this.lastError = null;
    this.lastCalibrationDiagnostics = null;
    this.notify();
    if (!enabled) void this.releaseProviderMemory();
  }

  get isEnabled(): boolean { return this.enabled; }
  get isBusy(): boolean { return this.providerBusy; }
  get hasStagedPostScanFrame(): boolean { return this.staged !== null; }
  get error(): string | null { return this.lastError ?? this.provider.error; }
  get status(): MonocularDepthProviderStatus {
    if (!this.enabled) return 'disabled';
    if (!this.provider.isSupported()) return 'unavailable';
    return this.provider.status;
  }
  get calibrationConfidence(): number { return this.calibrated?.calibration.confidence ?? 0; }
  get calibrationModel(): LogAffineDepthCalibration['model'] | null { return this.calibrated?.calibration.model ?? null; }
  get calibrationDiagnostics(): MonocularDepthCalibrationDiagnostics | null { return this.lastCalibrationDiagnostics; }

  reset(): void {
    this.epoch += 1;
    this.calibrated = null;
    this.staged = null;
    this.lastError = null;
    this.lastCalibrationDiagnostics = null;
    this.notify();
  }

  /** Release model/runtime memory and invalidate every per-scan result in flight. */
  async releaseMemory(): Promise<void> {
    this.epoch += 1;
    this.providerBusy = false;
    this.providerBusyEpoch = null;
    this.calibrated = null;
    this.staged = null;
    this.lastError = null;
    this.lastCalibrationDiagnostics = null;
    this.notify();
    await this.releaseProviderMemory();
  }

  private async releaseProviderMemory(): Promise<void> {
    if (this.provider.releaseMemory !== undefined) await this.provider.releaseMemory();
    else await this.provider.dispose();
  }

  stagePostScan(guidance: RgbDepthGuidanceFrame, depth: CpuDepthInformationLike): boolean {
    if (!this.enabled || !this.provider.isSupported()) return false;
    const anchors = collectPhysicalMetricAnchors(depth, guidance);
    if (anchors.length < MIN_CALIBRATION_INLIERS) return false;
    const physicalGuard = collectPhysicalDepthGuard(depth, guidance);
    const wasStaged = this.staged !== null;
    this.staged = {
      guidance: cloneGuidance(guidance),
      // These collectors already return newly-owned objects/typed arrays.
      anchors,
      physicalGuard,
    };
    if (!wasStaged) this.notify();
    return true;
  }

  /** Execute the staged ONNX prior only once after the XR session has ended. */
  async runStagedPostScan(): Promise<boolean> {
    const staged = this.staged;
    if (!this.enabled || staged === null || this.providerBusy || !this.provider.isSupported()) return false;
    const requestEpoch = this.epoch;
    this.staged = null;
    this.providerBusy = true;
    this.providerBusyEpoch = requestEpoch;
    this.notify();
    try {
      const prior = await this.provider.estimateRgba(
        guidanceAsRgba(staged.guidance),
        staged.guidance.width,
        staged.guidance.height,
        staged.guidance.capturedAtMs,
      );
      if (requestEpoch !== this.epoch || !this.enabled) return false;
      if (prior === null) {
        this.lastError = this.provider.error ?? 'Monocular prior inference returned no depth frame.';
        return false;
      }
      const detailed = calibrateMonocularDepthPriorDetailed(prior, staged.anchors);
      this.lastCalibrationDiagnostics = detailed.diagnostics;
      const calibration = detailed.calibration;
      if (requestEpoch !== this.epoch || !this.enabled) return false;
      if (
        calibration === null ||
        calibration.inliers < MIN_CALIBRATION_INLIERS ||
        calibration.confidence < MIN_CALIBRATION_CONFIDENCE
      ) {
        const selected = detailed.diagnostics.selectedModel ?? 'none';
        this.lastError = `Monocular prior did not pass physical-anchor calibration (model=${selected}).`;
        return false;
      }
      const viewFromWorld = staged.guidance.worldFromView === undefined
        ? null
        : rigidInverse(staged.guidance.worldFromView);
      const viewProjection = viewFromWorld === null || staged.guidance.projectionMatrix === undefined
        ? null
        : multiplyMat4(staged.guidance.projectionMatrix, viewFromWorld);
      this.calibrated = {
        frame: prior,
        calibration,
        captureGuidance: staged.guidance,
        physicalGuard: staged.physicalGuard,
        viewFromWorld,
        viewProjection,
      };
      this.lastError = null;
      return true;
    } catch (error) {
      if (requestEpoch === this.epoch && this.enabled) {
        this.lastError = error instanceof Error ? error.message : String(error);
      }
      return false;
    } finally {
      if (this.providerBusyEpoch === requestEpoch) {
        this.providerBusy = false;
        this.providerBusyEpoch = null;
        this.notify();
      }
    }
  }

  async maybeUpdate(
    guidance: RgbDepthGuidanceFrame,
    depth: CpuDepthInformationLike,
    _nowMs: number,
  ): Promise<boolean> {
    if (!this.stagePostScan(guidance, depth)) return false;
    return this.runStagedPostScan();
  }

  surfaceAgreement(
    u: number,
    v: number,
    physicalMeters: number,
    nowMs: number,
    currentGuidance?: RgbDepthGuidanceFrame,
  ): MonocularSurfaceAgreement {
    const calibrated = this.calibrated;
    const poseAligned = calibrated === null || currentGuidance === undefined || currentGuidance.worldFromView === undefined
      ? true
      : rgbGuidanceMatchesView(calibrated.captureGuidance, currentGuidance.worldFromView);
    if (
      !this.enabled || calibrated === null || !validPhysicalDepth(physicalMeters) ||
      nowMs - calibrated.frame.capturedAtMs > MAX_PRIOR_AGE_MS || !poseAligned
    ) {
      return {
        available: false,
        accepted: true,
        priorMeters: null,
        physicalMeters,
        disagreementMeters: null,
        calibrationConfidence: calibrated?.calibration.confidence ?? 0,
      };
    }

    const priorMeters = calibratedPriorDepthMeters(calibrated.frame, calibrated.calibration, u, v);
    if (priorMeters === null) {
      return {
        available: false,
        accepted: true,
        priorMeters: null,
        physicalMeters,
        disagreementMeters: null,
        calibrationConfidence: calibrated.calibration.confidence,
      };
    }

    const disagreementMeters = Math.abs(priorMeters - physicalMeters);
    const vetoThreshold = Math.max(0.32, physicalMeters * 0.30);
    return {
      available: true,
      accepted: disagreementMeters <= vetoThreshold,
      priorMeters,
      physicalMeters,
      disagreementMeters,
      calibrationConfidence: calibrated.calibration.confidence,
    };
  }

  /**
   * Allocation-light bulk path for the post-scan TSDF handoff. Unavailable,
   * invalid and occluded cases deliberately fail open; false means a calibrated
   * prior contradicted the same physically frontmost surface.
   */
  postScanAcceptsSurfaceXYZ(projectX: number, projectY: number, projectZ: number): boolean {
    const calibrated = this.calibrated;
    const viewFromWorld = calibrated?.viewFromWorld;
    const vp = calibrated?.viewProjection;
    if (!this.enabled || calibrated === null || viewFromWorld == null || vp == null) return true;
    if (!Number.isFinite(projectX) || !Number.isFinite(projectY) || !Number.isFinite(projectZ)) return true;

    const x = projectX;
    const y = projectZ;
    const z = -projectY;
    const viewZ = viewFromWorld[2] * x + viewFromWorld[6] * y + viewFromWorld[10] * z + viewFromWorld[14];
    const physicalMeters = -viewZ;
    if (!validPhysicalDepth(physicalMeters)) return true;

    const clipX = vp[0] * x + vp[4] * y + vp[8] * z + vp[12];
    const clipY = vp[1] * x + vp[5] * y + vp[9] * z + vp[13];
    const clipZ = vp[2] * x + vp[6] * y + vp[10] * z + vp[14];
    const clipW = vp[3] * x + vp[7] * y + vp[11] * z + vp[15];
    if (!Number.isFinite(clipX) || !Number.isFinite(clipY) || !Number.isFinite(clipZ) ||
        !Number.isFinite(clipW) || clipW <= 1e-6) return true;
    const ndcX = clipX / clipW;
    const ndcY = clipY / clipW;
    const ndcZ = clipZ / clipW;
    const u = (ndcX + 1) * 0.5;
    const v = (1 - ndcY) * 0.5;
    if (u < 0 || u > 1 || v < 0 || v > 1 || ndcZ < -1 || ndcZ > 1) return true;
    if (!physicalDepthGuardAllowsAiVeto(calibrated.physicalGuard, u, v, physicalMeters)) return true;

    const priorMeters = calibratedPriorDepthMeters(calibrated.frame, calibrated.calibration, u, v);
    if (priorMeters === null) return true;
    const vetoThreshold = Math.max(0.32, physicalMeters * 0.30);
    return Math.abs(priorMeters - physicalMeters) <= vetoThreshold;
  }

  postScanSurfaceAgreement(position: ProjectPoint): MonocularSurfaceAgreement {
    const calibrated = this.calibrated;
    const guidance = calibrated?.captureGuidance;
    if (
      calibrated === null || guidance === undefined ||
      guidance.projectionMatrix === undefined || guidance.worldFromView === undefined
    ) {
      return {
        available: false,
        accepted: true,
        priorMeters: null,
        physicalMeters: Number.NaN,
        disagreementMeters: null,
        calibrationConfidence: calibrated?.calibration.confidence ?? 0,
      };
    }
    const projected = projectProjectPointToNormalized(position, guidance.projectionMatrix, guidance.worldFromView);
    const physicalMeters = projected?.viewDepthMeters ?? Number.NaN;
    if (projected === null || !projected.visible || !validPhysicalDepth(physicalMeters)) {
      return {
        available: false,
        accepted: true,
        priorMeters: null,
        physicalMeters,
        disagreementMeters: null,
        calibrationConfidence: calibrated.calibration.confidence,
      };
    }
    // A monocular frame sees only the frontmost surface. Never let it reject a
    // different/occluded surfel that happens to reproject to the same RGB pixel.
    if (!physicalDepthGuardAllowsAiVeto(calibrated.physicalGuard, projected.u, projected.v, physicalMeters)) {
      return {
        available: false,
        accepted: true,
        priorMeters: null,
        physicalMeters,
        disagreementMeters: null,
        calibrationConfidence: calibrated.calibration.confidence,
      };
    }
    return this.surfaceAgreement(
      projected.u,
      projected.v,
      physicalMeters,
      calibrated.frame.capturedAtMs,
      guidance,
    );
  }

  async dispose(): Promise<void> {
    await this.releaseMemory();
    this.listeners.clear();
  }
}

export const optionalMonocularPrior = new MonocularPriorRuntime(
  new DepthAnythingV2WebGpuProvider(),
);
