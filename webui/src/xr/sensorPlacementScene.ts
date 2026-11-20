import type { LocalizationSessionGeometry, LocalizationSessionPodGeometry } from '../api/deviceApi';
import type { RegistrationArtifact } from './fiducial/registrationArtifact';
import {
  registrationApplicability,
  registrationBindingApplicability,
  registrationQualityOk,
  type RegistrationExpectation,
} from './registrationApplicability';
import { webXrLocalFloorToProject } from './scanViewportScene';
import type { TagSensorAnchor } from './fiducial/liveTagTracker';

export type SensorPlacementNode = 'MAIN' | 'G1' | 'G2' | 'G3' | 'G4';
export type SensorPlacementStatus = 'anchored' | 'cs_projected' | 'visual_only' | 'cs_unregistered';
export type SensorPlacementSource = 'channel_sounding' | 'channel_sounding_baro' | 'apriltag';
export type SensorPlacementPosition = readonly [number, number, number];

export interface SensorPlacementMarker {
  node: SensorPlacementNode;
  status: SensorPlacementStatus;
  source: SensorPlacementSource | null;
  positionScanM: SensorPlacementPosition | null;
  uncertaintyM: number | null;
  residualM: number | null;
  tagId: number | null;
  details: string;
  /** A tag position that is not yet confirmed from several viewpoints. Drawn faded and labelled; never treated as a confirmed anchor. */
  provisional?: boolean;
}

export interface SkippedSensorPlacement {
  node: SensorPlacementNode | null;
  reason: string;
}

export interface SensorPlacementScene {
  markers: SensorPlacementMarker[];
  skipped: SkippedSensorPlacement[];
}

const NODES: readonly SensorPlacementNode[] = ['MAIN', 'G1', 'G2', 'G3', 'G4'];
const UNDETERMINED_UNCERTAINTY_MM = 2147483647;

interface CsPosition {
  positionM: SensorPlacementPosition;
  source: SensorPlacementSource | null;
  uncertaintyM: number | null;
  residualM: number | null;
}

function nodeForId(nodeId: number | null | undefined): SensorPlacementNode | null {
  return Number.isInteger(nodeId) && nodeId! >= 0 && nodeId! <= 4 ? NODES[nodeId!] : null;
}

function finiteOptional(value: number | undefined): boolean {
  return value === undefined || Number.isFinite(value);
}

function metricM(valueMm: number | undefined, uncertainty = false): number | null {
  if (valueMm === undefined || (uncertainty && (valueMm === 0 || valueMm === UNDETERMINED_UNCERTAINTY_MM))) return null;
  return valueMm / 1000;
}

function readCsPosition(pod: LocalizationSessionPodGeometry, node: SensorPlacementNode): CsPosition | null {
  if (!finiteOptional(pod.uncertainty_mm) || !finiteOptional(pod.residual_mm)) return null;
  if ((pod.uncertainty_mm ?? 0) < 0 || (pod.residual_mm ?? 0) < 0) return null;

  let positionM: SensorPlacementPosition;
  if (node === 'MAIN') {
    const reported = [pod.x_mm, pod.y_mm, pod.z_mm].filter((value) => value !== undefined);
    if (reported.some((value) => !Number.isFinite(value))) return null;
    positionM = [0, 0, 0];
  } else {
    const coordinates = [pod.x_mm, pod.y_mm, pod.z_mm];
    if (coordinates.some((value) => value === undefined || !Number.isFinite(value))) return null;
    positionM = [pod.x_mm! / 1000, pod.y_mm! / 1000, pod.z_mm! / 1000];
  }

  return {
    positionM,
    source: pod.position_source === 'channel_sounding_baro'
      ? 'channel_sounding_baro'
      : pod.position_source === 'channel_sounding' ? 'channel_sounding' : null,
    uncertaintyM: metricM(pod.uncertainty_mm, true),
    residualM: metricM(pod.residual_mm),
  };
}

function bindingPosition(binding: RegistrationArtifact['bindings'][number]): SensorPlacementPosition | null {
  if (!Number.isFinite(binding.spread_m) || binding.spread_m < 0 || binding.position_scan_m.length !== 3 ||
      binding.position_scan_m.some((value) => !Number.isFinite(value))) return null;
  return [binding.position_scan_m[0], binding.position_scan_m[1], binding.position_scan_m[2]];
}

function registeredPosition(positionM: SensorPlacementPosition, matrix: number[]): SensorPlacementPosition {
  const x = positionM[0], y = positionM[1], z = positionM[2];
  const localFloor: SensorPlacementPosition = [
    matrix[0] * x + matrix[1] * y + matrix[2] * z + matrix[3],
    matrix[4] * x + matrix[5] * y + matrix[6] * z + matrix[7],
    matrix[8] * x + matrix[9] * y + matrix[10] * z + matrix[11],
  ];
  return webXrLocalFloorToProject(localFloor[0], localFloor[1], localFloor[2]);
}

function validTransform(artifact: RegistrationArtifact | null): number[] | null {
  const matrix = artifact?.sensor_to_scan;
  return artifact?.valid === true && (artifact.frame === undefined || artifact.frame === 'webxr_local_floor') &&
    registrationQualityOk(artifact) &&
    matrix !== null && matrix !== undefined && matrix.length === 16 &&
    matrix.every(Number.isFinite) ? matrix : null;
}

/**
 * Builds deterministic scan-frame markers from saved CS geometry and tag registration data.
 *
 * MAX-405: when `expected` (the geometry generation and scan identity the viewer is showing) is
 * given, a registration is used only if the shared applicability predicate accepts it; a
 * registration bound to another scan or geometry contributes neither a transform nor tag markers.
 * Callers that omit `expected` keep the structural checks only.
 */
export function buildSensorPlacementScene(
  geometry: LocalizationSessionGeometry | null,
  registrationInput: RegistrationArtifact | null,
  expected?: RegistrationExpectation,
): SensorPlacementScene {
  const skipped: SkippedSensorPlacement[] = [];
  let registration = registrationInput;
  if (expected !== undefined && registration !== null) {
    const binding = registrationBindingApplicability(registration, expected);
    if (!binding.ok) {
      skipped.push({ node: null, reason: `registration_${binding.reason}` });
      registration = null;
    }
  }
  const geometryByNode = new Map<SensorPlacementNode, CsPosition>();
  const invalidGeometryNodes = new Set<SensorPlacementNode>();

  if (geometry?.available) {
    let hasMainRow = false;
    for (const pod of geometry.pods) {
      const node = nodeForId(pod.node_id);
      if (node === 'MAIN') hasMainRow = true;
      if (node === null) {
        skipped.push({ node: null, reason: 'missing_or_invalid_node_id' });
        continue;
      }
      if (pod.computed !== true) {
        skipped.push({ node, reason: 'not_computed' });
        invalidGeometryNodes.add(node);
        continue;
      }
      if (pod.assigned === false) {
        skipped.push({ node, reason: 'not_assigned' });
        invalidGeometryNodes.add(node);
        continue;
      }
      const cs = readCsPosition(pod, node);
      if (cs === null) {
        skipped.push({ node, reason: 'missing_or_non_finite_geometry' });
        invalidGeometryNodes.add(node);
        continue;
      }
      if (geometryByNode.has(node)) {
        skipped.push({ node, reason: 'duplicate_geometry' });
        continue;
      }
      geometryByNode.set(node, cs);
    }
    if (!hasMainRow && !invalidGeometryNodes.has('MAIN')) {
      geometryByNode.set('MAIN', { positionM: [0, 0, 0], source: null, uncertaintyM: null, residualM: null });
    }
  }

  const transform = expected !== undefined && !registrationApplicability(registration, expected).ok
    ? null
    : validTransform(registration);
  const bindingByNode = new Map<SensorPlacementNode, { binding: RegistrationArtifact['bindings'][number]; position: SensorPlacementPosition }>();
  const invalidBindingNodes = new Set<SensorPlacementNode>();
  for (const binding of registration?.bindings ?? []) {
    const node = binding.node;
    const position = bindingPosition(binding);
    if (position === null) {
      skipped.push({ node, reason: 'invalid_tag_binding' });
      invalidBindingNodes.add(node);
    } else {
      bindingByNode.set(node, { binding, position });
    }
  }

  const markers: SensorPlacementMarker[] = [];
  for (const node of NODES) {
    const cs = geometryByNode.get(node);
    const binding = bindingByNode.get(node);
    if (transform !== null && cs !== undefined) {
      const perNode = registration?.per_node.find((item) => item.node === node) ?? null;
      if (perNode?.residualM !== null && perNode?.residualM !== undefined && !Number.isFinite(perNode.residualM)) {
        skipped.push({ node, reason: 'non_finite_registration_residual' });
        continue;
      }
      const anchored = binding !== undefined;
      markers.push({
        node,
        status: anchored ? 'anchored' : 'cs_projected',
        source: anchored ? 'apriltag' : cs.source,
        positionScanM: registeredPosition(cs.positionM, transform),
        uncertaintyM: cs.uncertaintyM,
        residualM: anchored ? perNode?.residualM ?? null : cs.residualM,
        tagId: binding?.binding.tag_id ?? null,
        details: anchored
          ? 'Channel-sounding geometry transformed by the saved AprilTag registration.'
          : 'Channel-sounding geometry projected through the saved registration; no tag is bound to this node.',
      });
      continue;
    }
    if (transform === null && binding !== undefined && !invalidGeometryNodes.has(node)) {
      markers.push({
        node,
        status: 'visual_only',
        source: 'apriltag',
        positionScanM: webXrLocalFloorToProject(binding.position[0], binding.position[1], binding.position[2]),
        uncertaintyM: binding.binding.spread_m,
        residualM: registration?.per_node.find((item) => item.node === node)?.residualM ?? null,
        tagId: binding.binding.tag_id,
        details: 'AprilTag position only; there is no valid sensor-to-scan transform for channel-sounding geometry.',
      });
      continue;
    }
    if (transform === null && cs !== undefined) {
      markers.push({
        node,
        status: 'cs_unregistered',
        source: cs.source,
        positionScanM: null,
        uncertaintyM: cs.uncertaintyM,
        residualM: cs.residualM,
        tagId: null,
        details: 'Channel-sounding geometry has no valid scan registration; it is shown only in the sensor-frame inset.',
      });
      continue;
    }
    if (invalidBindingNodes.has(node) || invalidGeometryNodes.has(node)) continue;
    skipped.push({ node, reason: 'missing_geometry' });
  }

  return { markers, skipped };
}

/**
 * Adds markers for tags confirmed during the live scan to the scene's markers, for the scan on screen. A node that already has a marker
 * (a saved registration or channel-sounding geometry) keeps it: the live anchor never replaces or overwrites that information.
 */
export function mergeLiveTagAnchorMarkers(markers: readonly SensorPlacementMarker[], anchors: readonly TagSensorAnchor[]): SensorPlacementMarker[] {
  const have = new Set(markers.filter((marker) => marker.positionScanM !== null).map((marker) => marker.node));
  const extra: SensorPlacementMarker[] = [];
  for (const anchor of anchors) {
    const node = anchor.node as SensorPlacementNode;
    if (!NODES.includes(node) || have.has(node)) continue;
    if (!anchor.positionScan.every(Number.isFinite)) continue;
    extra.push({
      node,
      status: 'visual_only',
      source: 'apriltag',
      positionScanM: webXrLocalFloorToProject(anchor.positionScan[0], anchor.positionScan[1], anchor.positionScan[2]),
      uncertaintyM: anchor.spreadM,
      residualM: null,
      tagId: anchor.tagId,
      details: `AprilTag position from the live scan (${anchor.observationCount} sightings from ${anchor.independentViewCount} viewpoints); ${anchor.confirmed ? 'confirmed' : 'preliminary, not yet confirmed'}; not registered to a sensor geometry.`,
      ...(anchor.confirmed ? {} : { provisional: true }),
    });
  }
  /* Nothing already in the scene is removed or changed: an unplaced channel-sounding marker keeps its information and the tag position stands beside it. */
  return [...markers, ...extra];
}
