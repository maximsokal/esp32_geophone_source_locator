import type { OccupancyRegistration } from '../occupancySolverArtifact';
import { localFloorMatrixToProject } from '../projectFrame';
import {
  registrationApplicability,
  registrationQualityOk,
  registrationTransformIssue,
  type RegistrationExpectation,
} from '../registrationApplicability';
import type { RegistrationNode, RegistrationNodeStatus, RegistrationTier } from './rigidRegistration';
import { TAG_BINDINGS, tagIdForNode, tagNodeForId, tagSizeMmForId } from './tagBindings.gen';

export const DEFAULT_REMOTE_TAG_SIZE_M = tagSizeMmForId(tagIdForNode('G1')) / 1000;
export const DEFAULT_MAIN_TAG_SIZE_M = tagSizeMmForId(tagIdForNode('MAIN')) / 1000;
export type RegistrationTagSizes = Record<'0' | '1' | '2' | '3' | '4', number>;

export function defaultRegistrationTagSizes(mainTagSizeM = DEFAULT_MAIN_TAG_SIZE_M, remoteTagSizeM = DEFAULT_REMOTE_TAG_SIZE_M): RegistrationTagSizes {
  const sizes = {} as RegistrationTagSizes;
  for (const binding of TAG_BINDINGS) {
    const id = String(binding.tag_id) as keyof RegistrationTagSizes;
    sizes[id] = arguments.length === 0
      ? binding.size_mm / 1000
      : binding.node === 'MAIN' ? mainTagSizeM : remoteTagSizeM;
  }
  return sizes;
}

export interface RegistrationBinding {
  tag_id: number;
  node: RegistrationNode;
  observation_count: number;
  inlier_count: number;
  position_scan_m: [number, number, number];
  spread_m: number;
}

export interface RegistrationArtifactNode {
  node: RegistrationNode;
  tagId: number;
  residualM: number | null;
  status: RegistrationNodeStatus;
}

/**
 * REMOTE_ONLY (no nRF MAIN): the centre of each geophone's AprilTag in the project frame (millimetres, +Z up), taken as the
 * geophone position with no offset. The firmware reads this block out of registration-v1.json and, for a session recorded
 * in REMOTE_ONLY and bound to the same geometry (`geometry_computed_at_ms`), uses it as the localization geometry. The
 * node id is the REMOTE's CS node id (G<k> = node k) and the tag is tag k.
 */
export interface RegistrationGeophonePosition {
  node_id: 1 | 2 | 3 | 4;
  tag_id: 1 | 2 | 3 | 4;
  x_mm: number;
  y_mm: number;
  z_mm: number;
}

/** What the tag centres say against what Channel Sounding measured, per pair (a report; nothing is corrected by it). */
export interface RegistrationBleResidual {
  node_a: 1 | 2 | 3 | 4;
  node_b: 1 | 2 | 3 | 4;
  tag_distance_mm: number;
  ble_distance_mm: number | null;
  residual_mm: number | null;
}

export interface RegistrationGeophonePositions {
  source: 'APRILTAG_CENTER_APPROX';
  frame: 'project_z_up_mm';
  positions: RegistrationGeophonePosition[];
  ble_residuals?: RegistrationBleResidual[];
}

export interface RegistrationArtifact {
  schema: 1;
  /** Missing only on legacy files; those matrices are WebXR local-floor. */
  frame?: 'webxr_local_floor';
  /**
   * MAX-405: identity (`scan-crc32:xxxxxxxx`) of the scan this registration is bound to. Absent or
   * null means a legacy/unbound file, which is never applicable to any scan (fail closed).
   */
  scan_frame_id?: string | null;
  source: 'apriltag';
  family: 'tag36h11';
  geometry_computed_at_ms: number;
  created_at_ms: number;
  tag_size_m: RegistrationTagSizes;
  bindings: RegistrationBinding[];
  sensor_to_scan: number[] | null;
  mirrored: boolean;
  residual_rms_m: number;
  threshold_m: number;
  per_node: RegistrationArtifactNode[];
  tier: RegistrationTier | null;
  valid: boolean;
  reason: string | null;
  /** Present only for a REMOTE_ONLY registration; an artifact without it is the MAIN-based kind and is unchanged. */
  geophone_positions?: RegistrationGeophonePositions;
}

const ROOT_KEYS = ['schema', 'frame', 'scan_frame_id', 'source', 'family', 'geometry_computed_at_ms', 'created_at_ms', 'tag_size_m', 'bindings', 'sensor_to_scan', 'mirrored', 'residual_rms_m', 'threshold_m', 'per_node', 'tier', 'valid', 'reason'];

function record(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

function exactKeys(value: Record<string, unknown>, keys: readonly string[], label: string): void {
  const actual = Object.keys(value).sort();
  const expected = [...keys].sort();
  if (actual.length !== expected.length || actual.some((key, index) => key !== expected[index])) throw new Error(`${label} has unexpected or missing fields.`);
}

function finite(value: unknown, label: string): number {
  if (typeof value !== 'number' || !Number.isFinite(value)) throw new Error(`${label} must be finite.`);
  return value;
}

function integer(value: unknown, label: string): number {
  const result = finite(value, label);
  if (!Number.isInteger(result)) throw new Error(`${label} must be an integer.`);
  return result;
}

function nodeForTagId(tagId: number): RegistrationNode {
  if (!Number.isInteger(tagId) || tagId < 0 || tagId > 4) throw new Error('Tag id must be between 0 and 4.');
  return tagNodeForId(tagId);
}

const MAX_GEOPHONE_COORDINATE_MM = 1_000_000;

function geophoneNodeId(value: unknown, label: string): 1 | 2 | 3 | 4 {
  const id = integer(value, label);
  if (id < 1 || id > 4) throw new Error(`${label} must be between 1 and 4.`);
  return id as 1 | 2 | 3 | 4;
}

function validateGeophonePositions(value: unknown): RegistrationGeophonePositions {
  if (!record(value)) throw new Error('geophone_positions must be an object.');
  exactKeys(value, ['source', 'frame', 'positions', ...(Object.prototype.hasOwnProperty.call(value, 'ble_residuals') ? ['ble_residuals'] : [])], 'geophone_positions');
  if (value.source !== 'APRILTAG_CENTER_APPROX') throw new Error('Unknown geophone position source.');
  if (value.frame !== 'project_z_up_mm') throw new Error('Unknown geophone position frame.');
  if (!Array.isArray(value.positions) || value.positions.length < 1 || value.positions.length > 4) throw new Error('geophone_positions.positions must hold one to four entries.');
  const seen = new Set<number>();
  const positions = value.positions.map((item): RegistrationGeophonePosition => {
    if (!record(item)) throw new Error('Each geophone position must be an object.');
    exactKeys(item, ['node_id', 'tag_id', 'x_mm', 'y_mm', 'z_mm'], 'geophone position');
    const nodeId = geophoneNodeId(item.node_id, 'geophone position node_id');
    const tagId = geophoneNodeId(item.tag_id, 'geophone position tag_id');
    if (tagId !== nodeId) throw new Error('A geophone position must come from its own tag.');
    if (seen.has(nodeId)) throw new Error('Duplicate geophone node ids are not allowed.');
    seen.add(nodeId);
    const coordinate = (key: 'x_mm' | 'y_mm' | 'z_mm'): number => {
      const result = integer(item[key], `geophone position ${key}`);
      if (Math.abs(result) > MAX_GEOPHONE_COORDINATE_MM) throw new Error(`geophone position ${key} is out of range.`);
      return result;
    };
    return { node_id: nodeId, tag_id: tagId, x_mm: coordinate('x_mm'), y_mm: coordinate('y_mm'), z_mm: coordinate('z_mm') };
  });
  const block: RegistrationGeophonePositions = { source: 'APRILTAG_CENTER_APPROX', frame: 'project_z_up_mm', positions };
  if (Object.prototype.hasOwnProperty.call(value, 'ble_residuals')) {
    if (!Array.isArray(value.ble_residuals) || value.ble_residuals.length > 6) throw new Error('ble_residuals must hold at most six pairs.');
    block.ble_residuals = value.ble_residuals.map((item): RegistrationBleResidual => {
      if (!record(item)) throw new Error('Each BLE residual must be an object.');
      exactKeys(item, ['node_a', 'node_b', 'tag_distance_mm', 'ble_distance_mm', 'residual_mm'], 'BLE residual');
      const nullable = (entry: unknown, label: string): number | null => (entry === null ? null : finite(entry, label));
      const ble = nullable(item.ble_distance_mm, 'ble_distance_mm');
      const residual = nullable(item.residual_mm, 'residual_mm');
      if ((ble === null) !== (residual === null)) throw new Error('A residual exists exactly when the BLE distance does.');
      return {
        node_a: geophoneNodeId(item.node_a, 'BLE residual node_a'), node_b: geophoneNodeId(item.node_b, 'BLE residual node_b'),
        tag_distance_mm: finite(item.tag_distance_mm, 'tag_distance_mm'), ble_distance_mm: ble, residual_mm: residual,
      };
    });
  }
  return block;
}

function validateMatrix(value: unknown): number[] | null {
  if (value === null) return null;
  if (!Array.isArray(value) || value.length !== 16) throw new Error('sensor_to_scan must be null or a 16-value row-major matrix.');
  const matrix = value.map((item) => finite(item, 'sensor_to_scan value'));
  if (matrix[12] !== 0 || matrix[13] !== 0 || matrix[14] !== 0 || matrix[15] !== 1) throw new Error('sensor_to_scan must have a homogeneous bottom row.');
  return matrix;
}

function validateArtifact(value: unknown): RegistrationArtifact {
  if (!record(value)) throw new Error('Registration artifact must be a JSON object.');
  const hasFrame = Object.prototype.hasOwnProperty.call(value, 'frame');
  const hasScanFrameId = Object.prototype.hasOwnProperty.call(value, 'scan_frame_id');
  const hasGeophonePositions = Object.prototype.hasOwnProperty.call(value, 'geophone_positions');
  exactKeys(value, [
    ...ROOT_KEYS.filter((key) => (hasFrame || key !== 'frame') && (hasScanFrameId || key !== 'scan_frame_id')),
    ...(hasGeophonePositions ? ['geophone_positions'] : []),
  ], 'Registration artifact');
  if (hasFrame && value.frame !== 'webxr_local_floor') {
    throw new Error('Unknown registration frame marker; expected webxr_local_floor.');
  }
  if (hasScanFrameId && value.scan_frame_id !== null && (typeof value.scan_frame_id !== 'string' || value.scan_frame_id.length === 0)) {
    throw new Error('scan_frame_id must be null or a non-empty string.');
  }
  if (value.schema !== 1) throw new Error('Unsupported registration artifact schema.');
  if (value.source !== 'apriltag') throw new Error('Unknown registration source.');
  if (value.family !== 'tag36h11') throw new Error('Unknown AprilTag family.');
  const geometryComputedAtMs = finite(value.geometry_computed_at_ms, 'geometry_computed_at_ms');
  if (geometryComputedAtMs <= 0) throw new Error('geometry_computed_at_ms must be greater than zero.');
  const createdAtMs = finite(value.created_at_ms, 'created_at_ms');
  if (createdAtMs < 0) throw new Error('created_at_ms must not be negative.');

  if (!record(value.tag_size_m)) throw new Error('tag_size_m must contain sizes for tag ids 0 through 4.');
  exactKeys(value.tag_size_m, ['0', '1', '2', '3', '4'], 'tag_size_m');
  const tagSizes = {} as RegistrationTagSizes;
  for (const id of ['0', '1', '2', '3', '4'] as const) {
    tagSizes[id] = finite(value.tag_size_m[id], `tag_size_m.${id}`);
    if (tagSizes[id] <= 0) throw new Error(`tag_size_m.${id} must be greater than zero.`);
  }

  if (!Array.isArray(value.bindings)) throw new Error('bindings must be an array.');
  const bindingIds = new Set<number>();
  const bindings = value.bindings.map((item): RegistrationBinding => {
    if (!record(item)) throw new Error('Each binding must be an object.');
    exactKeys(item, ['tag_id', 'node', 'observation_count', 'inlier_count', 'position_scan_m', 'spread_m'], 'binding');
    const tagId = integer(item.tag_id, 'binding.tag_id');
    const node = nodeForTagId(tagId);
    if (bindingIds.has(tagId)) throw new Error('Duplicate tag ids are not allowed.');
    bindingIds.add(tagId);
    if (item.node !== node) throw new Error('Binding node does not match its tag id.');
    const observationCount = integer(item.observation_count, 'binding.observation_count');
    const inlierCount = integer(item.inlier_count, 'binding.inlier_count');
    if (observationCount < 1 || inlierCount < 1 || inlierCount > observationCount) throw new Error('Binding observation counts are invalid.');
    if (!Array.isArray(item.position_scan_m) || item.position_scan_m.length !== 3) throw new Error('binding.position_scan_m must contain three values.');
    const position = item.position_scan_m.map((coordinate) => finite(coordinate, 'binding.position_scan_m')) as [number, number, number];
    const spreadM = finite(item.spread_m, 'binding.spread_m');
    if (spreadM < 0) throw new Error('binding.spread_m must not be negative.');
    return { tag_id: tagId, node, observation_count: observationCount, inlier_count: inlierCount, position_scan_m: position, spread_m: spreadM };
  });

  const matrix = validateMatrix(value.sensor_to_scan);
  const matrixIssue = matrix === null ? null : registrationTransformIssue(matrix);
  if (matrixIssue === 'invalid') throw new Error('sensor_to_scan rotation must be orthonormal with determinant +1.');
  if (typeof value.mirrored !== 'boolean') throw new Error('mirrored must be a boolean.');
  if (matrix !== null) {
    const determinant = matrix[0] * (matrix[5] * matrix[10] - matrix[6] * matrix[9]) - matrix[1] * (matrix[4] * matrix[10] - matrix[6] * matrix[8]) + matrix[2] * (matrix[4] * matrix[9] - matrix[5] * matrix[8]);
    if ((determinant < 0) !== value.mirrored) throw new Error('mirrored must match sensor_to_scan determinant.');
  }
  const residual = finite(value.residual_rms_m, 'residual_rms_m');
  const threshold = finite(value.threshold_m, 'threshold_m');
  if (residual < 0 || threshold < 0) throw new Error('Residual and threshold must not be negative.');

  if (!Array.isArray(value.per_node)) throw new Error('per_node must be an array.');
  const perNodeIds = new Set<number>();
  const perNode = value.per_node.map((item): RegistrationArtifactNode => {
    if (!record(item)) throw new Error('Each per_node entry must be an object.');
    exactKeys(item, ['node', 'tagId', 'residualM', 'status'], 'per_node entry');
    const tagId = integer(item.tagId, 'per_node.tagId');
    const node = nodeForTagId(tagId);
    if (item.node !== node || perNodeIds.has(tagId)) throw new Error('per_node ids and nodes must be unique and consistent.');
    perNodeIds.add(tagId);
    const residualM = item.residualM === null ? null : finite(item.residualM, 'per_node.residualM');
    if (residualM !== null && residualM < 0) throw new Error('per_node.residualM must not be negative.');
    if (item.status !== 'anchored' && item.status !== 'visual_only' && item.status !== 'provisional') throw new Error('Unknown per_node status.');
    return { node, tagId, residualM, status: item.status };
  });
  if (perNode.some((item) => !bindingIds.has(item.tagId))) throw new Error('Every per_node entry must have a matching binding.');

  if (value.tier !== null && value.tier !== 'full' && value.tier !== 'minimum') throw new Error('Unknown registration tier.');
  if (typeof value.valid !== 'boolean') throw new Error('valid must be a boolean.');
  if (value.reason !== null && (typeof value.reason !== 'string' || value.reason.length === 0)) throw new Error('reason must be null or a non-empty string.');
  if (matrixIssue === 'reflected_transform' && (value.valid !== false || value.reason !== 'reflected_transform')) {
    throw new Error('reflected_transform');
  }
  if (value.valid && (matrix === null || value.tier === null || value.reason !== null)) throw new Error('A valid artifact needs a transform, tier, and null reason.');
  if (!value.valid && (value.tier !== null || value.reason === null)) throw new Error('An invalid artifact needs a reason and null tier.');
  if (perNode.some((item) => item.status === 'anchored') && !value.valid) throw new Error('Invalid artifacts cannot contain anchored nodes.');
  if (value.valid && perNode.some((item) => item.status === 'provisional')) throw new Error('Valid artifacts cannot contain provisional nodes.');
  // MAX-405 quality invariant: a valid registration must be within its own threshold.
  if (value.valid && !registrationQualityOk({ residual_rms_m: residual, threshold_m: threshold })) {
    throw new Error('A valid artifact needs a positive threshold_m and residual_rms_m within it.');
  }

  return {
    schema: 1, frame: 'webxr_local_floor', scan_frame_id: (value.scan_frame_id as string | null | undefined) ?? null,
    source: 'apriltag', family: 'tag36h11',
    geometry_computed_at_ms: geometryComputedAtMs, created_at_ms: createdAtMs,
    tag_size_m: tagSizes, bindings, sensor_to_scan: matrix,
    mirrored: value.mirrored, residual_rms_m: residual, threshold_m: threshold,
    per_node: perNode, tier: value.tier, valid: value.valid, reason: value.reason,
    ...(hasGeophonePositions ? { geophone_positions: validateGeophonePositions(value.geophone_positions) } : {}),
  };
}

export function encodeRegistrationArtifact(artifact: RegistrationArtifact): string {
  return JSON.stringify(validateArtifact(artifact));
}

export function decodeRegistrationArtifact(json: string): RegistrationArtifact {
  return validateArtifact(JSON.parse(json) as unknown);
}

/**
 * The firmware stores only this compact transform copy in the occupancy header. Returns null for an
 * invalid, over-threshold or (when `expected` is given) inapplicable registration: a registration is
 * embedded only for the exact scan frame and geometry generation it was computed for.
 */
export function toOccupancyRegistration(
  artifact: RegistrationArtifact,
  expected?: RegistrationExpectation,
): OccupancyRegistration | null {
  try {
    const checked = validateArtifact(artifact);
    if (!checked.valid || checked.sensor_to_scan === null) return null;
    if (expected !== undefined && !registrationApplicability(checked, expected).ok) return null;
    return {
      source: 'apriltag',
      geometry_computed_at_ms: checked.geometry_computed_at_ms,
      sensor_to_scan: localFloorMatrixToProject(checked.sensor_to_scan),
      frame: 'project_z_up',
      residual_m: checked.residual_rms_m,
      threshold_m: checked.threshold_m,
    };
  } catch {
    return null;
  }
}
