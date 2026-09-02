import type { SensorSurfaceRelevanceAnalysis } from './sensorAwareSurfaceRelevance';
import type { OperationalSurfaceEnvelope } from './surfaceOperationalEnvelope';

export type SensorAwareSurfaceDisposition = 'retain' | 'quarantine' | 'promote-near-sensor';

export interface SensorAwareSurfaceDispositionEntry {
  componentId: number;
  disposition: SensorAwareSurfaceDisposition;
  nearestSensorId: string | null;
  nearestSensorDistanceMeters: number | null;
}

/**
 * Combine the existing physical connected-component quarantine policy with an
 * optional, frame-validated sensor relevance signal.
 *
 * Safety contract:
 * - this is metadata only; it never deletes or mutates physical surfels;
 * - already-retained physical components stay retained;
 * - a quarantined component may be promoted only when sensor relevance status is
 *   `ready`, which itself requires sensorFrameId === scanFrameId;
 * - lack of sensor proximity never demotes a component and never proves noise;
 * - `promote-near-sensor` means "important to preserve/rescan/model first", not
 *   "the mesh is geometrically certified".
 *
 * The policy can therefore be wired to real G1..G4 placements later without ever
 * allowing demo MAIN_LOCAL coordinates, GPS coordinates, or an uncalibrated frame
 * to erase measured geometry or alter the current TDOA observation set.
 */
export function classifySensorAwareSurfaceComponents(
  envelope: OperationalSurfaceEnvelope,
  relevance: SensorSurfaceRelevanceAnalysis | null | undefined,
): readonly SensorAwareSurfaceDispositionEntry[] {
  const retained = new Set(envelope.retainedComponentIds);
  const quarantined = new Set(envelope.quarantinedComponentIds);
  const relevant = relevance?.status === 'ready'
    ? new Set(relevance.relevantComponentIds)
    : new Set<number>();
  const relevanceByComponent = new Map(
    (relevance?.status === 'ready' ? relevance.components : [])
      .map((component) => [component.componentId, component] as const),
  );

  const componentIds = [...new Set([...retained, ...quarantined])].sort((a, b) => a - b);
  return componentIds.map((componentId) => {
    const sensor = relevanceByComponent.get(componentId);
    const disposition: SensorAwareSurfaceDisposition = retained.has(componentId)
      ? 'retain'
      : quarantined.has(componentId) && relevant.has(componentId)
        ? 'promote-near-sensor'
        : 'quarantine';
    return {
      componentId,
      disposition,
      nearestSensorId: sensor?.nearestSensorId ?? null,
      nearestSensorDistanceMeters: sensor?.nearestDistanceMeters ?? null,
    };
  });
}
