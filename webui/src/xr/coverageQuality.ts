export type CoverageQualityBand = 'weak' | 'improving' | 'stable';

export interface CoverageQualityCounts {
  weak: number;
  improving: number;
  stable: number;
}

export const COVERAGE_WEAK_MAX = 0.42;
export const COVERAGE_STABLE_MIN = 0.72;
const FLOAT32_THRESHOLD_EPSILON = 1e-6;

export function coverageQualityBand(confidence: number): CoverageQualityBand {
  const value = Number.isFinite(confidence) ? Math.max(0, Math.min(1, confidence)) : 0;
  // Confidence is stored in Float32Array. Values written as 0.42 can come back as
  // 0.419999986..., so compare with one tiny representational epsilon instead of
  // moving a point to a worse colour merely because of Float32 rounding.
  if (value + FLOAT32_THRESHOLD_EPSILON < COVERAGE_WEAK_MAX) return 'weak';
  if (value + FLOAT32_THRESHOLD_EPSILON < COVERAGE_STABLE_MIN) return 'improving';
  return 'stable';
}

export function countCoverageQualityBands(confidence: ArrayLike<number>): CoverageQualityCounts {
  const counts: CoverageQualityCounts = { weak: 0, improving: 0, stable: 0 };
  for (let index = 0; index < confidence.length; index += 1) {
    counts[coverageQualityBand(Number(confidence[index]))] += 1;
  }
  return counts;
}
