export interface MetricFeatureProbeInput {
  depthMeters: number;
  voxelSizeMeters: number;
  projectionScaleX: number;
  projectionScaleY: number;
  depthWidth: number;
  depthHeight: number;
}

export interface MetricFeatureProbeRadius {
  targetMeters: number;
  metersPerPixelX: number;
  metersPerPixelY: number;
  pixelRadiusX: number;
  pixelRadiusY: number;
  normalizedRadiusU: number;
  normalizedRadiusV: number;
}

const MIN_TARGET_METERS = 0.006;
const DEFAULT_MIN_TARGET_METERS = 0.008;
const MAX_DESIRED_TARGET_METERS = 0.050;
const MAX_LOCAL_TARGET_METERS = 0.075;
const TARGET_VOXEL_FRACTION = 0.55;
const MAX_TARGET_VOXEL_MULTIPLIER = 1.25;
const MAX_TARGET_DEPTH_FRACTION = 0.06;
const MAX_NORMALIZED_RADIUS = 0.06;
const MIN_RESOLVED_PIXEL_RADIUS = 1.25;

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
}

function positiveFinite(value: number): boolean {
  return Number.isFinite(value) && value > 0;
}

/**
 * Convert a desired physical feature-support radius into normalized depth-image
 * offsets. The policy is metric first: voxel size selects the useful local scale,
 * depth + XR projection encode FOV, and the actual depth-buffer resolution keeps
 * the probe at least one resolvable pixel away from the source sample.
 *
 * Returning null is intentional when a coarse/far depth buffer cannot resolve a
 * local probe without jumping outside the physically local neighbourhood. The
 * caller may still use its bounded generic edge fallback, but must not fabricate
 * feature density from a physically oversized image-space jump.
 */
export function metricFeatureProbeRadius(
  input: MetricFeatureProbeInput,
): MetricFeatureProbeRadius | null {
  const depthMeters = input.depthMeters;
  const voxelSizeMeters = Math.abs(input.voxelSizeMeters);
  const scaleX = Math.abs(input.projectionScaleX);
  const scaleY = Math.abs(input.projectionScaleY);
  const width = Math.floor(input.depthWidth);
  const height = Math.floor(input.depthHeight);

  if (
    !positiveFinite(depthMeters) || !positiveFinite(voxelSizeMeters) ||
    !positiveFinite(scaleX) || !positiveFinite(scaleY) ||
    width < 2 || height < 2
  ) return null;

  // For a standard perspective projection, visible half-width at metric depth d
  // is d / |projection[0]| and half-height is d / |projection[5]|.
  const metersPerPixelX = (2 * depthMeters) / (scaleX * width);
  const metersPerPixelY = (2 * depthMeters) / (scaleY * height);
  if (!positiveFinite(metersPerPixelX) || !positiveFinite(metersPerPixelY)) return null;

  const desiredMeters = clamp(
    voxelSizeMeters * TARGET_VOXEL_FRACTION,
    DEFAULT_MIN_TARGET_METERS,
    MAX_DESIRED_TARGET_METERS,
  );
  const resolvedFloorMeters = Math.max(metersPerPixelX, metersPerPixelY) * MIN_RESOLVED_PIXEL_RADIUS;
  const normalizedLimitMeters = Math.min(
    (2 * depthMeters * MAX_NORMALIZED_RADIUS) / scaleX,
    (2 * depthMeters * MAX_NORMALIZED_RADIUS) / scaleY,
  );
  const localCeilingMeters = Math.min(
    MAX_LOCAL_TARGET_METERS,
    voxelSizeMeters * MAX_TARGET_VOXEL_MULTIPLIER,
    depthMeters * MAX_TARGET_DEPTH_FRACTION,
    normalizedLimitMeters,
  );

  if (!positiveFinite(localCeilingMeters) || resolvedFloorMeters > localCeilingMeters) return null;
  const targetMeters = clamp(
    Math.max(desiredMeters, resolvedFloorMeters),
    MIN_TARGET_METERS,
    localCeilingMeters,
  );
  if (!positiveFinite(targetMeters)) return null;

  const normalizedRadiusU = targetMeters * scaleX / (2 * depthMeters);
  const normalizedRadiusV = targetMeters * scaleY / (2 * depthMeters);
  const pixelRadiusX = normalizedRadiusU * width;
  const pixelRadiusY = normalizedRadiusV * height;
  if (
    !positiveFinite(normalizedRadiusU) || !positiveFinite(normalizedRadiusV) ||
    !positiveFinite(pixelRadiusX) || !positiveFinite(pixelRadiusY)
  ) return null;

  return {
    targetMeters,
    metersPerPixelX,
    metersPerPixelY,
    pixelRadiusX,
    pixelRadiusY,
    normalizedRadiusU,
    normalizedRadiusV,
  };
}
