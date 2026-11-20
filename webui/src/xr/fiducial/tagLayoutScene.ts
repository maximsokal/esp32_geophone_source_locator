import type { SensorPlacementMarker, SensorPlacementNode } from '../sensorPlacementScene';
import { defaultRegistrationTagSizes, type RegistrationArtifact } from './registrationArtifact';
import type { TagPositionsArtifact } from './tagPositionsArtifact';

/*
 * What the saved tag layout (tag-positions-v1.json) puts into the 3D scan view of a recording: where MAIN and the geophones are, and the
 * transform that carries a localization result (sensor frame, MAIN at the origin, Z up) into the scan. The layout is computed on the phone and
 * only stored on the ESP32, so everything here is a pure function of the stored file.
 */

const NODE_NAMES: readonly SensorPlacementNode[] = ['MAIN', 'G1', 'G2', 'G3', 'G4'];

export function tagNodeName(nodeId: number): SensorPlacementNode | null {
  return Number.isInteger(nodeId) && nodeId >= 0 && nodeId < NODE_NAMES.length ? NODE_NAMES[nodeId] : null;
}

/** One marker per node of the layout, at layout + MAIN's position in the scan (project frame, metres, Z up). */
export function tagLayoutMarkers(artifact: TagPositionsArtifact): SensorPlacementMarker[] {
  const [mainX, mainY, mainZ] = artifact.main_scan_position_mm;
  const markers: SensorPlacementMarker[] = [];
  for (const node of artifact.nodes) {
    const name = tagNodeName(node.node_id);
    if (name === null) continue;
    markers.push({
      node: name,
      status: 'visual_only',
      source: 'apriltag',
      positionScanM: [(mainX + node.x_mm) / 1000, (mainY + node.y_mm) / 1000, (mainZ + node.z_mm) / 1000],
      uncertaintyM: node.uncertainty_mm / 1000,
      residualM: null,
      tagId: node.tag_id,
      details: `AprilTag position (${node.observations} sightings from ${node.views} viewpoints); ${node.confirmed ? 'confirmed' : 'preliminary, not yet confirmed'}.`,
      ...(node.confirmed ? {} : { provisional: true }),
    });
  }
  return markers;
}

/**
 * Sensor frame (MAIN origin, Z up) -> WebXR local floor, row-major 4x4, so that a sensor-frame point lands at layout + MAIN in the scan's
 * project frame. The project frame is X = XR x, Y = -XR z, Z = XR y, hence the rows. The rotation part is proper (determinant +1).
 */
export function tagLayoutSensorToLocalFloor(artifact: Pick<TagPositionsArtifact, 'main_scan_position_mm'>): number[] {
  const [px, py, pz] = artifact.main_scan_position_mm.map((value) => value / 1000);
  return [
    1, 0, 0, px,
    0, 0, 1, pz,
    0, -1, 0, -py,
    0, 0, 0, 1,
  ];
}

/**
 * A registration that exists only in memory: the translation the tag layout defines, shaped so the existing source-highlight code can place a
 * result of a tag-layout localization in the scan. Bound to the scan the tags were saved for and to the geometry generation of the run.
 * Null when the layout does not name its scan (it then belongs to no scan the viewer can show).
 */
export function tagLayoutRegistration(artifact: TagPositionsArtifact, geometryComputedAtMs: number): RegistrationArtifact | null {
  if (artifact.scan_frame_id === null || !Number.isFinite(geometryComputedAtMs)) return null;
  return {
    schema: 1,
    frame: 'webxr_local_floor',
    scan_frame_id: artifact.scan_frame_id,
    source: 'apriltag',
    family: 'tag36h11',
    geometry_computed_at_ms: geometryComputedAtMs,
    created_at_ms: artifact.created_at_ms,
    tag_size_m: defaultRegistrationTagSizes(),
    bindings: [],
    sensor_to_scan: tagLayoutSensorToLocalFloor(artifact),
    mirrored: false,
    residual_rms_m: 0,
    threshold_m: 1,
    per_node: [],
    tier: null,
    valid: true,
    reason: null,
  };
}
