export type RelativeDepthCalibrationModel = 'inverse-affine' | 'depth-affine' | 'log-affine';

export interface RelativeMetricSample {
  relativeDepth: number;
  metricMeters: number;
  weight: number;
}

export interface RelativeDepthCalibration {
  model: RelativeDepthCalibrationModel;
  scale: number;
  bias: number;
  inliers: number;
  rmsLogError: number;
  confidence: number;
}

export interface RelativeDepthCalibrationAttempt {
  model: RelativeDepthCalibrationModel;
  inliers: number;
  rmsLogError: number | null;
  confidence: number;
}

export interface RelativeDepthCalibrationDiagnostics {
  sampleCount: number;
  selectedModel: RelativeDepthCalibrationModel | null;
  attempts: readonly RelativeDepthCalibrationAttempt[];
}

const MODELS: readonly RelativeDepthCalibrationModel[] = ['inverse-affine', 'depth-affine', 'log-affine'];
const LOG_INLIER_THRESHOLD = 0.14;

function clamp(value: number, min: number, max: number): number {
  return Math.max(min, Math.min(max, value));
}

function coordinates(
  model: RelativeDepthCalibrationModel,
  sample: RelativeMetricSample,
): readonly [number, number] | null {
  const relative = sample.relativeDepth;
  const metric = sample.metricMeters;
  if (!Number.isFinite(relative) || relative <= 1e-6 || !Number.isFinite(metric) || metric <= 0.08) return null;
  if (model === 'inverse-affine') return [relative, 1 / metric];
  if (model === 'depth-affine') return [relative, metric];
  return [Math.log(relative), Math.log(metric)];
}

export function metricDepthFromRelative(
  relativeDepth: number,
  calibration: RelativeDepthCalibration,
): number | null {
  if (!Number.isFinite(relativeDepth) || relativeDepth <= 1e-6) return null;
  let metric: number;
  if (calibration.model === 'inverse-affine') {
    const inverse = calibration.scale * relativeDepth + calibration.bias;
    if (!Number.isFinite(inverse) || inverse <= 1e-5) return null;
    metric = 1 / inverse;
  } else if (calibration.model === 'depth-affine') {
    metric = calibration.scale * relativeDepth + calibration.bias;
  } else {
    metric = Math.exp(calibration.scale * Math.log(relativeDepth) + calibration.bias);
  }
  return Number.isFinite(metric) && metric > 0.08 && metric < 12 ? metric : null;
}

function logResidual(
  sample: RelativeMetricSample,
  model: RelativeDepthCalibrationModel,
  scale: number,
  bias: number,
): number | null {
  const predicted = metricDepthFromRelative(sample.relativeDepth, {
    model,
    scale,
    bias,
    inliers: 0,
    rmsLogError: 0,
    confidence: 0,
  });
  if (predicted === null) return null;
  return Math.abs(Math.log(predicted) - Math.log(sample.metricMeters));
}

function weightedLine(
  samples: readonly RelativeMetricSample[],
  model: RelativeDepthCalibrationModel,
  robustWeights?: readonly number[],
): { scale: number; bias: number } | null {
  let w = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (let index = 0; index < samples.length; index += 1) {
    const pair = coordinates(model, samples[index]);
    if (pair === null) continue;
    const weight = Math.max(0, samples[index].weight * (robustWeights?.[index] ?? 1));
    if (weight <= 0) continue;
    const [x, y] = pair;
    w += weight;
    sx += x * weight;
    sy += y * weight;
    sxx += x * x * weight;
    sxy += x * y * weight;
  }
  const determinant = w * sxx - sx * sx;
  if (w <= 1e-8 || Math.abs(determinant) <= 1e-10) return null;
  const scale = (w * sxy - sx * sy) / determinant;
  const bias = (sy * sxx - sx * sxy) / determinant;
  return Number.isFinite(scale) && Number.isFinite(bias) ? { scale, bias } : null;
}

function fitModel(
  input: readonly RelativeMetricSample[],
  model: RelativeDepthCalibrationModel,
): RelativeDepthCalibration | null {
  const samples = input.filter((sample) => coordinates(model, sample) !== null);
  if (samples.length < 6) return null;

  let best: { scale: number; bias: number; inlierIndices: number[]; weightedError: number } | null = null;
  const maxTrials = Math.min(96, samples.length * 4);
  for (let trial = 0; trial < maxTrials; trial += 1) {
    const first = trial % samples.length;
    const second = (trial * 7 + Math.floor(samples.length / 2) + 1) % samples.length;
    if (first === second) continue;
    const a = coordinates(model, samples[first]);
    const b = coordinates(model, samples[second]);
    if (a === null || b === null) continue;
    const dx = b[0] - a[0];
    if (Math.abs(dx) < 1e-7) continue;
    const scale = (b[1] - a[1]) / dx;
    const bias = a[1] - scale * a[0];
    if (!Number.isFinite(scale) || !Number.isFinite(bias)) continue;
    if (model === 'log-affine' && (Math.abs(scale) < 0.20 || Math.abs(scale) > 4.5)) continue;

    const inlierIndices: number[] = [];
    let weightedError = 0;
    for (let index = 0; index < samples.length; index += 1) {
      const residual = logResidual(samples[index], model, scale, bias);
      if (residual !== null && residual <= LOG_INLIER_THRESHOLD) {
        inlierIndices.push(index);
        weightedError += residual * samples[index].weight;
      }
    }
    if (
      best === null ||
      inlierIndices.length > best.inlierIndices.length ||
      (inlierIndices.length === best.inlierIndices.length && weightedError < best.weightedError)
    ) {
      best = { scale, bias, inlierIndices, weightedError };
    }
  }

  const minimumInliers = Math.max(6, Math.ceil(samples.length * 0.40));
  if (best === null || best.inlierIndices.length < minimumInliers) return null;
  const active = best.inlierIndices.map((index) => samples[index]);
  let fit = weightedLine(active, model);
  if (fit === null) return null;

  for (let iteration = 0; iteration < 4; iteration += 1) {
    const residuals = active
      .map((sample) => logResidual(sample, model, fit!.scale, fit!.bias) ?? Number.POSITIVE_INFINITY)
      .filter(Number.isFinite)
      .sort((a, b) => a - b);
    const median = residuals[Math.floor(residuals.length * 0.5)] ?? 0.03;
    const huber = Math.max(0.035, median * 2.2);
    const robustWeights = active.map((sample) => {
      const residual = logResidual(sample, model, fit!.scale, fit!.bias) ?? Number.POSITIVE_INFINITY;
      return residual <= huber ? 1 : huber / Math.max(residual, 1e-9);
    });
    fit = weightedLine(active, model, robustWeights) ?? fit;
  }

  if (model === 'log-affine' && (Math.abs(fit.scale) < 0.20 || Math.abs(fit.scale) > 4.5)) return null;
  let squared = 0;
  let valid = 0;
  for (const sample of active) {
    const residual = logResidual(sample, model, fit.scale, fit.bias);
    if (residual === null) continue;
    squared += residual * residual;
    valid += 1;
  }
  if (valid < minimumInliers) return null;
  const rms = Math.sqrt(squared / valid);
  const inlierRatio = valid / samples.length;
  const confidence = clamp(
    inlierRatio * 0.62 + clamp(1 - rms / 0.12, 0, 1) * 0.38,
    0,
    1,
  );
  return {
    model,
    scale: fit.scale,
    bias: fit.bias,
    inliers: valid,
    rmsLogError: rms,
    confidence,
  };
}

export function calibrateRelativeDepthToMetricDetailed(
  samples: readonly RelativeMetricSample[],
): { calibration: RelativeDepthCalibration | null; diagnostics: RelativeDepthCalibrationDiagnostics } {
  const attempts: RelativeDepthCalibrationAttempt[] = [];
  let selected: RelativeDepthCalibration | null = null;
  for (const model of MODELS) {
    const calibration = fitModel(samples, model);
    attempts.push({
      model,
      inliers: calibration?.inliers ?? 0,
      rmsLogError: calibration?.rmsLogError ?? null,
      confidence: calibration?.confidence ?? 0,
    });
    if (
      calibration !== null && (
        selected === null ||
        calibration.inliers > selected.inliers ||
        (calibration.inliers === selected.inliers && calibration.rmsLogError < selected.rmsLogError)
      )
    ) selected = calibration;
  }
  return {
    calibration: selected,
    diagnostics: {
      sampleCount: samples.length,
      selectedModel: selected?.model ?? null,
      attempts,
    },
  };
}

export function calibrateRelativeDepthToMetric(
  samples: readonly RelativeMetricSample[],
): RelativeDepthCalibration | null {
  return calibrateRelativeDepthToMetricDetailed(samples).calibration;
}
