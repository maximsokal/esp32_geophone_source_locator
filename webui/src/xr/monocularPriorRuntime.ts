import type { CpuDepthInformationLike } from './depthPointCloudV2';
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
import {
  sampleRgbGuidance,
  type RgbDepthGuidanceFrame,
} from './rgbDepthGuidance';

interface CalibratedPrior {
  frame: MonocularDepthPriorFrame;
  calibration: LogAffineDepthCalibration;
}

export interface MonocularSurfaceAgreement {
  available: boolean;
  accepted: boolean;
  priorMeters: number | null;
  physicalMeters: number;
  disagreementMeters: number | null;
  calibrationConfidence: number;
}

const UPDATE_INTERVAL_MS = 6500;
const MAX_PRIOR_AGE_MS = 9000;
const MIN_CALIBRATION_CONFIDENCE = 0.66;
const MIN_CALIBRATION_INLIERS = 10;
const ANCHOR_COLUMNS = 8;
const ANCHOR_ROWS = 6;

function validPhysicalDepth(value: number): boolean {
  return Number.isFinite(value) && value >= 0.18 && value <= 8;
}

function guidanceAsRgba(frame: RgbDepthGuidanceFrame): Uint8Array {
  const rgba = new Uint8Array(frame.width * frame.height * 4);
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
  private busy = false;
  private epoch = 1;
  private lastAttemptMs = Number.NEGATIVE_INFINITY;
  private calibrated: CalibratedPrior | null = null;
  private lastError: string | null = null;

  constructor(private readonly provider: MonocularDepthProvider) {}

  setEnabled(enabled: boolean): void {
    if (this.enabled === enabled) return;
    this.enabled = enabled;
    // Any in-flight inference belongs to the previous enable/scan epoch. The
    // provider may still finish internally, but its result must never mutate
    // current geometry state.
    this.epoch += 1;
    this.busy = false;
    this.lastAttemptMs = Number.NEGATIVE_INFINITY;
    this.calibrated = null;
    this.lastError = null;
  }

  get isEnabled(): boolean { return this.enabled; }
  get isBusy(): boolean { return this.busy; }
  get error(): string | null { return this.lastError ?? this.provider.error; }
  get status(): MonocularDepthProviderStatus {
    if (!this.enabled) return 'disabled';
    if (!this.provider.isSupported()) return 'unavailable';
    return this.provider.status;
  }
  get calibrationConfidence(): number { return this.calibrated?.calibration.confidence ?? 0; }

  reset(): void {
    this.epoch += 1;
    this.busy = false;
    this.lastAttemptMs = Number.NEGATIVE_INFINITY;
    this.calibrated = null;
    this.lastError = null;
  }

  async maybeUpdate(
    guidance: RgbDepthGuidanceFrame,
    depth: CpuDepthInformationLike,
    nowMs: number,
  ): Promise<boolean> {
    if (!this.enabled || this.busy || !this.provider.isSupported()) return false;
    if (nowMs - this.lastAttemptMs < UPDATE_INTERVAL_MS) return false;
    const anchors = collectPhysicalMetricAnchors(depth, guidance);
    if (anchors.length < MIN_CALIBRATION_INLIERS) return false;

    const requestEpoch = this.epoch;
    this.lastAttemptMs = nowMs;
    this.busy = true;
    try {
      const prior = await this.provider.estimateRgba(
        guidanceAsRgba(guidance),
        guidance.width,
        guidance.height,
        guidance.capturedAtMs,
      );
      if (requestEpoch !== this.epoch || !this.enabled || prior === null) return false;
      const calibration = calibrateMonocularDepthPrior(prior, anchors);
      if (requestEpoch !== this.epoch || !this.enabled) return false;
      if (
        calibration === null ||
        calibration.inliers < MIN_CALIBRATION_INLIERS ||
        calibration.confidence < MIN_CALIBRATION_CONFIDENCE
      ) {
        this.lastError = 'Monocular prior did not pass physical-anchor calibration.';
        return false;
      }
      this.calibrated = { frame: prior, calibration };
      this.lastError = null;
      return true;
    } catch (error) {
      if (requestEpoch === this.epoch && this.enabled) {
        this.lastError = error instanceof Error ? error.message : String(error);
      }
      return false;
    } finally {
      if (requestEpoch === this.epoch) this.busy = false;
    }
  }

  surfaceAgreement(
    u: number,
    v: number,
    physicalMeters: number,
    nowMs: number,
  ): MonocularSurfaceAgreement {
    const calibrated = this.calibrated;
    if (
      !this.enabled || calibrated === null || !validPhysicalDepth(physicalMeters) ||
      nowMs - calibrated.frame.capturedAtMs > MAX_PRIOR_AGE_MS
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

  dispose(): void {
    this.epoch += 1;
    this.provider.dispose();
    this.reset();
  }
}

export const optionalMonocularPrior = new MonocularPriorRuntime(
  new DepthAnythingV2WebGpuProvider(),
);
