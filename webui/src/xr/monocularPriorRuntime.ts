import type { CpuDepthInformationLike } from './depthPointCloudV2';
import type { ProjectPoint } from './manualControlPoints';
import {
  calibrateMonocularDepthPrior,
  calibratedPriorDepthMeters,
  DepthAnythingV2WebGpuProvider,
  type LogAffineDepthCalibration,
  type MetricDepthAnchor,
  type MonocularDepthPriorFrame,
  type MonocularDepthProvider,
  type MonocularDepthProviderStatus,
} from './monocularDepthPrior';
import { projectProjectPointToNormalized } from './projectionMath';
import {
  rgbGuidanceMatchesView,
  sampleRgbGuidance,
  type RgbDepthGuidanceFrame,
} from './rgbDepthGuidance';

interface CalibratedPrior {
  frame: MonocularDepthPriorFrame;
  calibration: LogAffineDepthCalibration;
  /** RGB frame/pose whose pixels produced this monocular depth map. */
  captureGuidance: RgbDepthGuidanceFrame;
}

interface StagedPostScanPrior {
  guidance: RgbDepthGuidanceFrame;
  anchors: MetricDepthAnchor[];
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
    return frame.rgba.length === expected ? frame.rgba.slice() : frame.rgba.slice(0, expected);
  }
  // Synthetic/tests/older callers may only carry luma. Grayscale fallback keeps
  // them compatible, while production Raw Camera Access preserves true RGBA.
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
      // Do not calibrate against the exact RGB/depth boundaries that are most
      // likely to contain ARCore flying pixels.
      if (visual !== null && visual.edgeStrength >= 0.20) continue;
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

export class MonocularPriorRuntime {
  private enabled = false;
  private providerBusy = false;
  private epoch = 1;
  private calibrated: CalibratedPrior | null = null;
  private staged: StagedPostScanPrior | null = null;
  private lastError: string | null = null;

  constructor(private readonly provider: MonocularDepthProvider) {}

  setEnabled(enabled: boolean): void {
    if (this.enabled === enabled) return;
    this.enabled = enabled;
    // Any in-flight inference belongs to the previous enable/scan epoch. The
    // provider may still finish internally, but its result must never mutate
    // current geometry state.
    this.epoch += 1;
    this.calibrated = null;
    this.staged = null;
    this.lastError = null;
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

  reset(): void {
    this.epoch += 1;
    this.calibrated = null;
    this.staged = null;
    this.lastError = null;
  }

  /**
   * Cheap live-scan path: copy one pose-synchronised RGB frame plus physical
   * ARCore anchors. No ONNX model is loaded or executed here. The latest valid
   * keyframe replaces the previous staging packet so live WebXR never competes
   * with a multi-second mobile WebGPU inference.
   */
  stagePostScan(guidance: RgbDepthGuidanceFrame, depth: CpuDepthInformationLike): boolean {
    if (!this.enabled || !this.provider.isSupported()) return false;
    const anchors = collectPhysicalMetricAnchors(depth, guidance);
    if (anchors.length < MIN_CALIBRATION_INLIERS) return false;
    this.staged = { guidance: cloneGuidance(guidance), anchors: anchors.map((anchor) => ({ ...anchor })) };
    return true;
  }

  /** Execute the staged ONNX prior only after the XR session has ended. */
  async runStagedPostScan(): Promise<boolean> {
    const staged = this.staged;
    if (!this.enabled || staged === null || this.providerBusy || !this.provider.isSupported()) return false;
    const requestEpoch = this.epoch;
    this.providerBusy = true;
    try {
      const prior = await this.provider.estimateRgba(
        guidanceAsRgba(staged.guidance),
        staged.guidance.width,
        staged.guidance.height,
        staged.guidance.capturedAtMs,
      );
      if (requestEpoch !== this.epoch || !this.enabled || prior === null) return false;
      const calibration = calibrateMonocularDepthPrior(prior, staged.anchors);
      if (requestEpoch !== this.epoch || !this.enabled) return false;
      if (
        calibration === null ||
        calibration.inliers < MIN_CALIBRATION_INLIERS ||
        calibration.confidence < MIN_CALIBRATION_CONFIDENCE
      ) {
        this.lastError = 'Monocular prior did not pass physical-anchor calibration.';
        return false;
      }
      this.calibrated = { frame: prior, calibration, captureGuidance: staged.guidance };
      this.lastError = null;
      return true;
    } catch (error) {
      if (requestEpoch === this.epoch && this.enabled) {
        this.lastError = error instanceof Error ? error.message : String(error);
      }
      return false;
    } finally {
      this.providerBusy = false;
    }
  }

  /**
   * Backward-compatible explicit helper used by isolated tests/tools. Production
   * live XR uses stagePostScan() and calls runStagedPostScan() only after Finish.
   */
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
    // A calibrated monocular model is only a gross-outlier veto. Normal ARCore
    // noise and legitimate fine geometry remain controlled by physical depth.
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
   * Post-scan TSDF-only check. The metric depth remains the measured 3D point's
   * view depth; ONNX can only reject a grossly inconsistent surfel visible from
   * the staged RGB pose. Points outside that view are untouched.
   */
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
    // Pass capture time and capture pose so the live-age/pose safeguards remain
    // satisfied without weakening them for normal runtime calls.
    return this.surfaceAgreement(
      projected.u,
      projected.v,
      physicalMeters,
      calibrated.frame.capturedAtMs,
      guidance,
    );
  }

  dispose(): void {
    this.epoch += 1;
    this.provider.dispose();
    this.reset();
  }
}

export const optionalMonocularPrior = new MonocularPriorRuntime(
  new DepthAnythingV2WebGpuProvider(),
);