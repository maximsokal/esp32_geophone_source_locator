import type { OccupancyRegistration } from '../occupancySolverArtifact';
import type { RegistrationNode, RegistrationNodeStatus, RegistrationTier } from './rigidRegistration';

export const DEFAULT_REMOTE_TAG_SIZE_M = 0.032;
export const DEFAULT_MAIN_TAG_SIZE_M = 0.032;
export type RegistrationTagSizes = Record<'0' | '1' | '2' | '3' | '4', number>;

export function defaultRegistrationTagSizes(mainTagSizeM = DEFAULT_MAIN_TAG_SIZE_M, remoteTagSizeM = DEFAULT_REMOTE_TAG_SIZE_M): RegistrationTagSizes {
  return { '0': mainTagSizeM, '1': remoteTagSizeM, '2': remoteTagSizeM, '3': remoteTagSizeM, '4': remoteTagSizeM };
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

export interface RegistrationArtifact {
  schema: 1;
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
}

const NODES: readonly RegistrationNode[] = ['MAIN', 'G1', 'G2', 'G3', 'G4'];
const ROOT_KEYS = ['schema', 'source', 'family', 'geometry_computed_at_ms', 'created_at_ms', 'tag_size_m', 'bindings', 'sensor_to_scan', 'mirrored', 'residual_rms_m', 'threshold_m', 'per_node', 'tier', 'valid', 'reason'];

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
  return NODES[tagId];
}

function validateMatrix(value: unknown): number[] | null {
  if (value === null) return null;
  if (!Array.isArray(value) || value.length !== 16) throw new Error('sensor_to_scan must be null or a 16-value row-major matrix.');
  const matrix = value.map((item) => finite(item, 'sensor_to_scan value'));
  if (matrix[12] !== 0 || matrix[13] !== 0 || matrix[14] !== 0 || matrix[15] !== 1) throw new Error('sensor_to_scan must have a homogeneous bottom row.');
  const rotation = [matrix[0], matrix[1], matrix[2], matrix[4], matrix[5], matrix[6], matrix[8], matrix[9], matrix[10]];
  for (let row = 0; row < 3; row += 1) for (let other = 0; other < 3; other += 1) {
    const dot = rotation[row * 3] * rotation[other * 3] + rotation[row * 3 + 1] * rotation[other * 3 + 1] + rotation[row * 3 + 2] * rotation[other * 3 + 2];
    if (Math.abs(dot - (row === other ? 1 : 0)) > 1e-3) throw new Error('sensor_to_scan rotation must be orthonormal.');
  }
  const determinant = rotation[0] * (rotation[4] * rotation[8] - rotation[5] * rotation[7]) - rotation[1] * (rotation[3] * rotation[8] - rotation[5] * rotation[6]) + rotation[2] * (rotation[3] * rotation[7] - rotation[4] * rotation[6]);
  if (Math.abs(Math.abs(determinant) - 1) > 1e-3) throw new Error('sensor_to_scan rotation determinant must be plus or minus one.');
  return matrix;
}

function validateArtifact(value: unknown): RegistrationArtifact {
  if (!record(value)) throw new Error('Registration artifact must be a JSON object.');
  exactKeys(value, ROOT_KEYS, 'Registration artifact');
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
  if (value.valid && (matrix === null || value.tier === null || value.reason !== null)) throw new Error('A valid artifact needs a transform, tier, and null reason.');
  if (!value.valid && (value.tier !== null || value.reason === null)) throw new Error('An invalid artifact needs a reason and null tier.');
  if (perNode.some((item) => item.status === 'anchored') && !value.valid) throw new Error('Invalid artifacts cannot contain anchored nodes.');
  if (value.valid && perNode.some((item) => item.status === 'provisional')) throw new Error('Valid artifacts cannot contain provisional nodes.');

  return {
    schema: 1, source: 'apriltag', family: 'tag36h11',
    geometry_computed_at_ms: geometryComputedAtMs, created_at_ms: createdAtMs,
    tag_size_m: tagSizes, bindings, sensor_to_scan: matrix,
    mirrored: value.mirrored, residual_rms_m: residual, threshold_m: threshold,
    per_node: perNode, tier: value.tier, valid: value.valid, reason: value.reason,
  };
}

export function encodeRegistrationArtifact(artifact: RegistrationArtifact): string {
  return JSON.stringify(validateArtifact(artifact));
}

export function decodeRegistrationArtifact(json: string): RegistrationArtifact {
  return validateArtifact(JSON.parse(json) as unknown);
}

/** The firmware stores only this compact transform copy in the occupancy header. */
export function toOccupancyRegistration(artifact: RegistrationArtifact): OccupancyRegistration | null {
  try {
    const checked = validateArtifact(artifact);
    if (!checked.valid || checked.sensor_to_scan === null) return null;
    return {
      source: 'apriltag',
      geometry_computed_at_ms: checked.geometry_computed_at_ms,
      sensor_to_scan: [...checked.sensor_to_scan],
      residual_m: checked.residual_rms_m,
    };
  } catch {
    return null;
  }
}
