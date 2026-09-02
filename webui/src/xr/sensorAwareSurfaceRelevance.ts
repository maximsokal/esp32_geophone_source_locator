import type { SurfaceComponentAnalysis, SurfaceComponentPoint } from './surfaceComponents';

export interface SensorSurfacePlacement {
  id: string;
  position: SurfaceComponentPoint;
  /** 1-sigma-ish placement uncertainty supplied by the positioning pipeline. */
  uncertaintyMeters?: number;
  /** Unlocked/provisional sensors cannot protect geometry from quarantine. */
  locked?: boolean;
}

export interface SensorSurfaceContext {
  /** Coordinate frame used by the physical scan surfels. */
  scanFrameId: string;
  /** Coordinate frame used by all sensor positions in this context. */
  sensorFrameId: string;
  sensors: readonly SensorSurfacePlacement[];
}

export type SensorSurfaceRelevanceStatus = 'unavailable' | 'frame-mismatch' | 'ready';

export interface SensorComponentRelevance {
  componentId: number;
  nearestSensorId: string;
  nearestDistanceMeters: number;
  relevanceGateMeters: number;
  relevant: boolean;
}

export interface SensorSurfaceRelevanceAnalysis {
  status: SensorSurfaceRelevanceStatus;
  relevantComponentIds: readonly number[];
  components: readonly SensorComponentRelevance[];
}

const BASE_SENSOR_RELEVANCE_METERS = 0.16;
const MAX_SENSOR_UNCERTAINTY_METERS = 0.40;
const MAX_SENSOR_RELEVANCE_METERS = 0.55;

function finitePoint(point: SurfaceComponentPoint): boolean {
  return Number.isFinite(point[0]) && Number.isFinite(point[1]) && Number.isFinite(point[2]);
}

function distance(
  ax: number,
  ay: number,
  az: number,
  b: SurfaceComponentPoint,
): number {
  return Math.hypot(ax - b[0], ay - b[1], az - b[2]);
}

function sensorGate(sensor: SensorSurfacePlacement, voxelSizeMeters: number): number {
  const voxel = Number.isFinite(voxelSizeMeters)
    ? Math.max(0.025, Math.min(0.10, Math.abs(voxelSizeMeters)))
    : 0.04;
  const uncertainty = Number.isFinite(sensor.uncertaintyMeters)
    ? Math.max(0, Math.min(MAX_SENSOR_UNCERTAINTY_METERS, Number(sensor.uncertaintyMeters)))
    : 0;
  // A sensor is mechanically attached to / placed on the measured structure, so
  // allow a few surfel voxels plus the explicitly supplied placement uncertainty.
  // This is deliberately only a relevance radius, never an occupancy/mesh gate.
  return Math.min(
    MAX_SENSOR_RELEVANCE_METERS,
    Math.max(BASE_SENSOR_RELEVANCE_METERS, voxel * 3.5) + uncertainty,
  );
}

/**
 * Sensor-aware relevance for disconnected physical surface components.
 *
 * Safety contract:
 * - sensor and scan coordinates MUST name the exact same calibrated frame;
 * - only locked, finite sensor positions participate;
 * - distance is measured to actual component surfels, not to the component
 *   centroid, so a sensor near the edge of a large slab is not missed;
 * - a positive match is a one-way PRIORITY signal for rescan/future propagation
 *   modelling. A negative match is never evidence that physical geometry is fake;
 * - this function never mutates the surface component analysis, occupancy, TSDF
 *   eligibility, TDOA observations or UNKNOWN search envelope.
 *
 * This lets the WebXR pipeline accept future calibrated G1..G4 positions without
 * accidentally consuming demo MAIN_LOCAL coordinates or GPS coordinates whose
 * transforms into the current XR reference space are unknown.
 */
export function analyzeSensorSurfaceRelevance(
  positions: ArrayLike<number>,
  analysis: SurfaceComponentAnalysis,
  context: SensorSurfaceContext | null | undefined,
  voxelSizeMeters: number,
): SensorSurfaceRelevanceAnalysis {
  if (
    context === null || context === undefined ||
    context.sensors.length === 0 ||
    context.scanFrameId.trim().length === 0 ||
    context.sensorFrameId.trim().length === 0
  ) {
    return { status: 'unavailable', relevantComponentIds: [], components: [] };
  }
  if (context.scanFrameId !== context.sensorFrameId) {
    return { status: 'frame-mismatch', relevantComponentIds: [], components: [] };
  }

  const sensors = context.sensors.filter((sensor) =>
    sensor.locked !== false && sensor.id.trim().length > 0 && finitePoint(sensor.position)
  );
  const labels = analysis.componentIdByPoint;
  if (sensors.length === 0 || labels === undefined || labels.length === 0) {
    return { status: 'unavailable', relevantComponentIds: [], components: [] };
  }

  const pointCount = Math.min(labels.length, Math.floor(positions.length / 3));
  const nearestByComponent = new Map<number, { sensor: SensorSurfacePlacement; distance: number }>();

  for (let index = 0; index < pointCount; index += 1) {
    const componentId = Number(labels[index]);
    if (!Number.isInteger(componentId) || componentId < 0) continue;
    const offset = index * 3;
    const x = Number(positions[offset]);
    const y = Number(positions[offset + 1]);
    const z = Number(positions[offset + 2]);
    if (!Number.isFinite(x) || !Number.isFinite(y) || !Number.isFinite(z)) continue;

    for (const sensor of sensors) {
      const candidateDistance = distance(x, y, z, sensor.position);
      const existing = nearestByComponent.get(componentId);
      if (existing === undefined || candidateDistance < existing.distance) {
        nearestByComponent.set(componentId, { sensor, distance: candidateDistance });
      }
    }
  }

  const components: SensorComponentRelevance[] = [];
  for (const component of analysis.components) {
    const nearest = nearestByComponent.get(component.id);
    if (nearest === undefined) continue;
    const relevanceGateMeters = sensorGate(nearest.sensor, voxelSizeMeters);
    components.push({
      componentId: component.id,
      nearestSensorId: nearest.sensor.id,
      nearestDistanceMeters: nearest.distance,
      relevanceGateMeters,
      relevant: nearest.distance <= relevanceGateMeters,
    });
  }
  components.sort((left, right) => left.componentId - right.componentId);

  return {
    status: 'ready',
    relevantComponentIds: components
      .filter((component) => component.relevant)
      .map((component) => component.componentId),
    components,
  };
}
