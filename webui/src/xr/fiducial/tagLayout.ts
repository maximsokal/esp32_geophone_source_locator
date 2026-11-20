import { xrToProject, type TagSensorAnchor } from './liveTagTracker';

/**
 * The sensor layout derived from AprilTag positions, computed on the PHONE (the ESP32 only stores and uses the finished result).
 *
 * Frame `sensor_main_origin_z_up_mm`: MAIN is the origin, X/Y horizontal, Z up (the scan's own vertical, so heights come from gravity,
 * not from a barometer), true handedness. Axes keep the scan's azimuth, so the layout maps onto the 3D scan by a translation (the MAIN
 * tag position, kept in `mainScanPositionMm`). Triangles are built from MAIN; every extra tag adds triangles.
 *
 * What the triangles are for, honestly: all tags of one scan already share one rigid frame, so the triangles carry the same positions
 * as the raw tag centres. They give (1) a frame that does not depend on how the scan is registered, (2) a geometry-quality figure
 * (a thin triangle fixes the layout badly), (3) detection of two tags that cannot be two sensors, and (4) a per-node comparison with an
 * independent reference layout (Channel Sounding) that exposes a mixed-up tag. The accuracy itself comes from the fused tag positions.
 */
export const TAG_LAYOUT_FRAME = 'sensor_main_origin_z_up_mm' as const;
/** A preliminary (unconfirmed) tag joins the layout only if its position spread is at most this. */
export const TAG_LAYOUT_PROVISIONAL_MAX_SPREAD_MM = 50;
/** MAIN needs at least one triangle at least this tall (smallest altitude) to fix a layout. */
export const TAG_LAYOUT_MIN_TRIANGLE_HEIGHT_MM = 30;
/** Two sensors closer than this are one tag seen twice or two tags mixed up (the firmware uses the same limit). */
export const TAG_LAYOUT_MIN_SEPARATION_MM = 10;
const MAX_COORDINATE_MM = 200_000;

export interface TagLayoutNode {
  nodeId: number;
  tagId: number;
  xMm: number;
  yMm: number;
  zMm: number;
  /** 1-sigma 3D position uncertainty relative to MAIN (0 for MAIN, the origin). */
  uncertaintyMm: number;
  confirmed: boolean;
  observations: number;
  views: number;
}

export interface TagLayoutTriangle {
  /** Node ids, ascending. */
  nodes: readonly [number, number, number];
  includesMain: boolean;
  /** Smallest altitude of the triangle in mm; near 0 means nearly collinear. */
  heightMm: number;
}

export type TagLayoutExclusionReason = 'provisional_spread_too_high' | 'too_close_to_another_tag' | 'invalid_position' | 'unknown_node';
export interface TagLayoutExclusion { tagId: number; node: string; reason: TagLayoutExclusionReason }

export interface TagLayout {
  frame: typeof TAG_LAYOUT_FRAME;
  /** MAIN first, then ascending node id. */
  nodes: TagLayoutNode[];
  /** MAIN's position in the scan's project frame (X, Y, Z up), mm: layout + this = scan position. */
  mainScanPositionMm: [number, number, number];
  triangles: TagLayoutTriangle[];
  weakestMainTriangleHeightMm: number;
  excluded: TagLayoutExclusion[];
}

export type TagLayoutFailure = 'no_main' | 'too_few_tags' | 'degenerate_triangles';
export interface TagLayoutResult { layout: TagLayout | null; reason: TagLayoutFailure | null; excluded: TagLayoutExclusion[] }

export interface TagLayoutOptions {
  provisionalMaxSpreadMm?: number;
  minTriangleHeightMm?: number;
  minSeparationMm?: number;
}

type Vec3 = readonly [number, number, number];
const sub = (a: Vec3, b: Vec3): Vec3 => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const norm = (a: Vec3): number => Math.hypot(a[0], a[1], a[2]);
const cross = (a: Vec3, b: Vec3): Vec3 => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];

/** MAIN -> 0, G<k> -> k (the firmware's node id for the pod); null for anything else. */
export function nodeIdForName(node: string): number | null {
  if (node === 'MAIN') return 0;
  const match = /^G([1-4])$/.exec(node);
  return match === null ? null : Number(match[1]);
}

/** Smallest altitude of the triangle (a, b, c): twice the area over the longest side. */
export function triangleHeightMm(a: Vec3, b: Vec3, c: Vec3): number {
  const area2 = norm(cross(sub(b, a), sub(c, a)));
  const longest = Math.max(norm(sub(b, a)), norm(sub(c, b)), norm(sub(a, c)));
  return longest <= 0 ? 0 : area2 / longest;
}

export function buildTagLayout(anchors: readonly TagSensorAnchor[], options: TagLayoutOptions = {}): TagLayoutResult {
  const provisionalMax = options.provisionalMaxSpreadMm ?? TAG_LAYOUT_PROVISIONAL_MAX_SPREAD_MM;
  const minHeight = options.minTriangleHeightMm ?? TAG_LAYOUT_MIN_TRIANGLE_HEIGHT_MM;
  const minSeparation = options.minSeparationMm ?? TAG_LAYOUT_MIN_SEPARATION_MM;
  const excluded: TagLayoutExclusion[] = [];

  interface Candidate { anchor: TagSensorAnchor; nodeId: number; scanMm: Vec3 }
  const candidates: Candidate[] = [];
  for (const anchor of anchors) {
    const nodeId = nodeIdForName(anchor.node);
    if (nodeId === null) { excluded.push({ tagId: anchor.tagId, node: anchor.node, reason: 'unknown_node' }); continue; }
    if (!anchor.positionScan.every(Number.isFinite) || !Number.isFinite(anchor.spreadM)) {
      excluded.push({ tagId: anchor.tagId, node: anchor.node, reason: 'invalid_position' }); continue;
    }
    if (!anchor.confirmed && anchor.spreadM * 1000 > provisionalMax) {
      excluded.push({ tagId: anchor.tagId, node: anchor.node, reason: 'provisional_spread_too_high' }); continue;
    }
    const projectM = xrToProject(anchor.positionScan);
    candidates.push({ anchor, nodeId, scanMm: [projectM[0] * 1000, projectM[1] * 1000, projectM[2] * 1000] });
  }
  // One tag per node: keep the confirmed / better-observed reading if a node somehow appears twice.
  const byNode = new Map<number, Candidate>();
  for (const candidate of candidates) {
    const held = byNode.get(candidate.nodeId);
    if (held === undefined || (candidate.anchor.confirmed && !held.anchor.confirmed) ||
        (candidate.anchor.confirmed === held.anchor.confirmed && candidate.anchor.observationCount > held.anchor.observationCount)) {
      byNode.set(candidate.nodeId, candidate);
    }
  }
  const main = byNode.get(0);
  if (main === undefined) return { layout: null, reason: 'no_main', excluded };

  // Two sensors cannot stand in one place: such a pair is a tag seen twice or two tags mixed up. Drop the less certain one.
  const kept = new Map(byNode);
  const ids = [...byNode.keys()].sort((a, b) => a - b);
  for (let i = 0; i < ids.length; i += 1) {
    for (let j = i + 1; j < ids.length; j += 1) {
      const a = kept.get(ids[i]); const b = kept.get(ids[j]);
      if (a === undefined || b === undefined || norm(sub(a.scanMm, b.scanMm)) >= minSeparation) continue;
      const dropB = ids[i] === 0 || Number(a.anchor.confirmed) > Number(b.anchor.confirmed) ||
        (a.anchor.confirmed === b.anchor.confirmed && a.anchor.spreadM <= b.anchor.spreadM);
      const dropped = dropB ? b : a;
      kept.delete(dropped.nodeId);
      excluded.push({ tagId: dropped.anchor.tagId, node: dropped.anchor.node, reason: 'too_close_to_another_tag' });
    }
  }

  const nodes: TagLayoutNode[] = [...kept.values()].sort((a, b) => a.nodeId - b.nodeId).map((candidate) => {
    const relative = sub(candidate.scanMm, main.scanMm);
    const spreadMm = candidate.anchor.spreadM * 1000;
    const mainSpreadMm = main.anchor.spreadM * 1000;
    return {
      nodeId: candidate.nodeId, tagId: candidate.anchor.tagId,
      xMm: Math.round(relative[0]), yMm: Math.round(relative[1]), zMm: Math.round(relative[2]),
      uncertaintyMm: candidate.nodeId === 0 ? 0 : Math.max(1, Math.round(Math.hypot(spreadMm, mainSpreadMm))),
      confirmed: candidate.anchor.confirmed, observations: candidate.anchor.observationCount, views: candidate.anchor.independentViewCount,
    };
  });
  if (nodes.some((node) => Math.max(Math.abs(node.xMm), Math.abs(node.yMm), Math.abs(node.zMm)) > MAX_COORDINATE_MM)) {
    return { layout: null, reason: 'too_few_tags', excluded };
  }
  if (nodes.length < 3) return { layout: null, reason: 'too_few_tags', excluded };

  const triangles: TagLayoutTriangle[] = [];
  for (let i = 0; i < nodes.length; i += 1) for (let j = i + 1; j < nodes.length; j += 1) for (let k = j + 1; k < nodes.length; k += 1) {
    const point = (n: TagLayoutNode): Vec3 => [n.xMm, n.yMm, n.zMm];
    triangles.push({
      nodes: [nodes[i].nodeId, nodes[j].nodeId, nodes[k].nodeId],
      includesMain: nodes[i].nodeId === 0,
      heightMm: triangleHeightMm(point(nodes[i]), point(nodes[j]), point(nodes[k])),
    });
  }
  const mainTriangles = triangles.filter((triangle) => triangle.includesMain);
  const weakest = Math.min(...mainTriangles.map((triangle) => triangle.heightMm));
  if (!(Math.max(...mainTriangles.map((triangle) => triangle.heightMm)) >= minHeight)) {
    return { layout: null, reason: 'degenerate_triangles', excluded };
  }
  return {
    layout: {
      frame: TAG_LAYOUT_FRAME, nodes, mainScanPositionMm: [Math.round(main.scanMm[0]), Math.round(main.scanMm[1]), Math.round(main.scanMm[2])],
      triangles, weakestMainTriangleHeightMm: Math.round(weakest), excluded,
    },
    reason: null, excluded,
  };
}

export interface ReferenceNode { nodeId: number; xMm: number; yMm: number; zMm: number }
export interface LayoutAgreement {
  pairs: { a: number; b: number; tagMm: number; referenceMm: number; deltaMm: number }[];
  /** Per node: mean |tag distance - reference distance| over its pairs. */
  meanDeltaMm: Record<number, number>;
  /** Nodes whose pairs disagree with the reference by more than the tolerance on average: a mixed-up tag or a bad reference. */
  suspectNodes: number[];
}

/**
 * Compares the tag layout with an independent layout (Channel Sounding) by pairwise distances, which need no common orientation.
 * The reference is only a witness: it never changes the tag layout, and a disagreement is reported, not "corrected".
 */
export function compareLayoutWithReference(layout: TagLayout, reference: readonly ReferenceNode[], toleranceMm = 1000): LayoutAgreement {
  const refById = new Map(reference.map((node) => [node.nodeId, node]));
  const pairs: LayoutAgreement['pairs'] = [];
  for (let i = 0; i < layout.nodes.length; i += 1) for (let j = i + 1; j < layout.nodes.length; j += 1) {
    const a = layout.nodes[i]; const b = layout.nodes[j];
    const ra = refById.get(a.nodeId); const rb = refById.get(b.nodeId);
    if (ra === undefined || rb === undefined) continue;
    const tagMm = Math.hypot(a.xMm - b.xMm, a.yMm - b.yMm, a.zMm - b.zMm);
    const referenceMm = Math.hypot(ra.xMm - rb.xMm, ra.yMm - rb.yMm, ra.zMm - rb.zMm);
    pairs.push({ a: a.nodeId, b: b.nodeId, tagMm, referenceMm, deltaMm: Math.abs(tagMm - referenceMm) });
  }
  const sums = new Map<number, { total: number; count: number }>();
  for (const pair of pairs) for (const id of [pair.a, pair.b]) {
    const held = sums.get(id) ?? { total: 0, count: 0 };
    held.total += pair.deltaMm; held.count += 1; sums.set(id, held);
  }
  const meanDeltaMm: Record<number, number> = {};
  for (const [id, { total, count }] of sums) meanDeltaMm[id] = total / count;
  const suspectNodes = Object.entries(meanDeltaMm).filter(([id, mean]) => Number(id) !== 0 && mean > toleranceMm).map(([id]) => Number(id));
  return { pairs, meanDeltaMm, suspectNodes };
}

export interface TagLayoutQuality { nodes: number; confirmed: number }

export function tagLayoutQuality(layout: TagLayout): TagLayoutQuality {
  return { nodes: layout.nodes.length, confirmed: layout.nodes.filter((node) => node.confirmed).length };
}

/**
 * Whether a layout computed for a scan may replace the one already saved for that same scan. A later, shorter look at the placement (a
 * rejected continuation, a quick second pass) sees fewer tags and fewer of them confirmed; it must never wipe out the better layout.
 * No saved layout: anything replaces nothing. No new layout: it never removes a saved one for the same scan.
 */
export function tagLayoutReplaces(candidate: TagLayout | null, saved: TagLayoutQuality | null): boolean {
  if (saved === null) return true;
  if (candidate === null) return false;
  const quality = tagLayoutQuality(candidate);
  return quality.confirmed >= saved.confirmed && quality.nodes >= saved.nodes;
}
