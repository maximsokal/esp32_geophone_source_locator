import { TAG_LAYOUT_FRAME, type TagLayout, type TagLayoutNode } from './tagLayout';

/**
 * `tag-positions-v1.json`: the finished tag-derived sensor layout, saved on the ESP32 next to the session's other artifacts
 * (scan store kind `tags`). It is computed on the phone; the ESP32 only stores it, shows it beside the Channel Sounding / barometer
 * layout, and lets the localization use it as an alternative source. It never replaces those values.
 *
 * Positions are final, in the MAIN-origin sensor frame, integer millimetres. The firmware parser enforces the same limits.
 */
export const TAG_POSITIONS_SCHEMA = 1;
export const TAG_POSITIONS_KIND = 'tag_positions';
export const TAG_POSITIONS_MAX_BYTES = 4096;
const MAX_COORDINATE_MM = 200_000;
const MAX_UNCERTAINTY_MM = 100_000;

export interface TagPositionsArtifact {
  schema: typeof TAG_POSITIONS_SCHEMA;
  kind: typeof TAG_POSITIONS_KIND;
  frame: typeof TAG_LAYOUT_FRAME;
  /** The scan these tags were seen in (scan-crc32:...), or null when the scan identity is not known yet. */
  scan_frame_id: string | null;
  created_at_ms: number;
  /** The frozen Channel Sounding geometry generation (newest pod computed_at_ms) this layout was built against; null/absent = unknown (a staged scan: no frozen geometry exists yet); the ESP32 then uses it as before, and refuses it only when an epoch is present and differs. */
  geometry_computed_at_ms?: number | null;
  /** MAIN in the scan's project frame (mm): layout + this = position in the 3D scan. */
  main_scan_position_mm: [number, number, number];
  weakest_main_triangle_height_mm: number;
  nodes: {
    node_id: number; tag_id: number; x_mm: number; y_mm: number; z_mm: number;
    uncertainty_mm: number; confirmed: boolean; observations: number; views: number;
  }[];
}

const ROOT_KEYS = ['schema', 'kind', 'frame', 'scan_frame_id', 'created_at_ms', 'geometry_computed_at_ms', 'main_scan_position_mm', 'weakest_main_triangle_height_mm', 'nodes'];
const NODE_KEYS = ['node_id', 'tag_id', 'x_mm', 'y_mm', 'z_mm', 'uncertainty_mm', 'confirmed', 'observations', 'views'];

export function tagPositionsFromLayout(layout: TagLayout, scanFrameId: string | null, createdAtMs: number, geometryComputedAtMs: number | null = null): TagPositionsArtifact {
  return {
    schema: TAG_POSITIONS_SCHEMA, kind: TAG_POSITIONS_KIND, frame: TAG_LAYOUT_FRAME, scan_frame_id: scanFrameId, created_at_ms: Math.round(createdAtMs), geometry_computed_at_ms: geometryComputedAtMs,
    main_scan_position_mm: [...layout.mainScanPositionMm],
    weakest_main_triangle_height_mm: layout.weakestMainTriangleHeightMm,
    nodes: layout.nodes.map((node: TagLayoutNode) => ({
      node_id: node.nodeId, tag_id: node.tagId, x_mm: node.xMm, y_mm: node.yMm, z_mm: node.zMm,
      uncertainty_mm: node.uncertaintyMm, confirmed: node.confirmed, observations: node.observations, views: node.views,
    })),
  };
}

function isInt(value: unknown, min: number, max: number): value is number {
  return typeof value === 'number' && Number.isInteger(value) && value >= min && value <= max;
}

/** Strict validation: throws on anything the firmware parser would refuse. Returns the artifact when valid. */
export function validateTagPositions(value: unknown): TagPositionsArtifact {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) throw new Error('tag positions must be an object.');
  const root = value as Record<string, unknown>;
  for (const key of Object.keys(root)) if (!ROOT_KEYS.includes(key)) throw new Error(`unknown tag positions field: ${key}`);
  if (root.schema !== TAG_POSITIONS_SCHEMA) throw new Error('unsupported tag positions schema.');
  if (root.kind !== TAG_POSITIONS_KIND) throw new Error('not a tag positions file.');
  if (root.frame !== TAG_LAYOUT_FRAME) throw new Error('unsupported tag positions frame.');
  if (root.scan_frame_id !== null && (typeof root.scan_frame_id !== 'string' || !/^[A-Za-z0-9:_-]{1,80}$/.test(root.scan_frame_id))) throw new Error('invalid scan_frame_id.');
  if (!isInt(root.created_at_ms, 0, Number.MAX_SAFE_INTEGER)) throw new Error('invalid created_at_ms.');
  if (root.geometry_computed_at_ms !== undefined && root.geometry_computed_at_ms !== null && !isInt(root.geometry_computed_at_ms, 1, Number.MAX_SAFE_INTEGER)) throw new Error('invalid geometry_computed_at_ms.');
  const main = root.main_scan_position_mm;
  if (!Array.isArray(main) || main.length !== 3 || !main.every((v) => isInt(v, -MAX_COORDINATE_MM, MAX_COORDINATE_MM))) throw new Error('invalid main_scan_position_mm.');
  if (!isInt(root.weakest_main_triangle_height_mm, 0, MAX_COORDINATE_MM * 2)) throw new Error('invalid weakest_main_triangle_height_mm.');
  const nodes = root.nodes;
  if (!Array.isArray(nodes) || nodes.length < 3 || nodes.length > 5) throw new Error('tag positions need 3 to 5 nodes.');
  const seen = new Set<number>();
  for (const item of nodes) {
    if (typeof item !== 'object' || item === null || Array.isArray(item)) throw new Error('invalid node.');
    const node = item as Record<string, unknown>;
    for (const key of Object.keys(node)) if (!NODE_KEYS.includes(key)) throw new Error(`unknown node field: ${key}`);
    if (!isInt(node.node_id, 0, 4) || seen.has(node.node_id)) throw new Error('invalid or repeated node_id.');
    seen.add(node.node_id);
    if (node.tag_id !== node.node_id) throw new Error('tag_id must equal node_id (MAIN is tag 0, G<k> is tag k).');
    for (const axis of ['x_mm', 'y_mm', 'z_mm']) if (!isInt(node[axis], -MAX_COORDINATE_MM, MAX_COORDINATE_MM)) throw new Error(`invalid ${axis}.`);
    if (!isInt(node.uncertainty_mm, 0, MAX_UNCERTAINTY_MM)) throw new Error('invalid uncertainty_mm.');
    if (typeof node.confirmed !== 'boolean') throw new Error('invalid confirmed flag.');
    if (!isInt(node.observations, 0, 100_000) || !isInt(node.views, 0, 100_000)) throw new Error('invalid observation counts.');
  }
  const list = nodes as TagPositionsArtifact['nodes'];
  const mainNode = list.find((node) => node.node_id === 0);
  if (mainNode === undefined) throw new Error('MAIN (node 0) is required.');
  if (mainNode.x_mm !== 0 || mainNode.y_mm !== 0 || mainNode.z_mm !== 0 || mainNode.uncertainty_mm !== 0) throw new Error('MAIN must be the origin with zero uncertainty.');
  for (let i = 0; i < list.length; i += 1) for (let j = i + 1; j < list.length; j += 1) {
    if (Math.hypot(list[i].x_mm - list[j].x_mm, list[i].y_mm - list[j].y_mm, list[i].z_mm - list[j].z_mm) < 10) throw new Error('two nodes closer than 10 mm.');
  }
  return value as TagPositionsArtifact;
}

export function encodeTagPositions(artifact: TagPositionsArtifact): string {
  validateTagPositions(artifact);
  const text = JSON.stringify(artifact);
  if (new TextEncoder().encode(text).byteLength > TAG_POSITIONS_MAX_BYTES) throw new Error('tag positions are too large.');
  return text;
}

export function decodeTagPositions(text: string): TagPositionsArtifact {
  if (new TextEncoder().encode(text).byteLength > TAG_POSITIONS_MAX_BYTES) throw new Error('tag positions are too large.');
  return validateTagPositions(JSON.parse(text) as unknown);
}
