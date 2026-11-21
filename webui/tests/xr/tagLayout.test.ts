import { describe, expect, it } from 'vitest';
import { xrToProject, type TagSensorAnchor } from '../../src/xr/fiducial/liveTagTracker';
import {
  buildTagLayout, compareLayoutWithReference, tagAnchorsToBaseFrame, nodeIdForName, tagLayoutReplaces, triangleHeightMm, TAG_LAYOUT_FRAME,
} from '../../src/xr/fiducial/tagLayout';

/** An anchor in the WebXR local-floor frame (metres), as the live scan produces it. */
const anchor = (tagId: number, node: string, xr: [number, number, number], confirmed = true, spreadM = 0.003, observationCount = 8, independentViewCount = 4): TagSensorAnchor =>
  ({ tagId, node, positionScan: xr, spreadM, observationCount, independentViewCount, confirmed });

const MAIN = anchor(0, 'MAIN', [1, 1, -1]);
const G1 = anchor(1, 'G1', [1.5, 1, -1]);      // 0.5 m along XR +X
const G2 = anchor(2, 'G2', [1, 1, -1.8]);      // 0.8 m along XR -Z = project +Y
const G3 = anchor(3, 'G3', [0.9, 1.2, -1.5]);  // 0.1 m back, 0.2 m up

describe('tag layout in the sensor frame (MAIN at the origin, Z up)', () => {
  it('maps node names to firmware node ids', () => {
    expect(['MAIN', 'G1', 'G2', 'G3', 'G4', 'G5', 'X'].map(nodeIdForName)).toEqual([0, 1, 2, 3, 4, null, null]);
  });

  it('puts MAIN at the origin and every other tag relative to it, in the project axes (X, Y horizontal, Z up)', () => {
    const { layout, reason } = buildTagLayout([G3, MAIN, G2, G1]);
    expect(reason).toBeNull();
    expect(layout!.frame).toBe(TAG_LAYOUT_FRAME);
    expect(layout!.nodes.map((n) => [n.nodeId, n.xMm, n.yMm, n.zMm])).toEqual([[0, 0, 0, 0], [1, 500, 0, 0], [2, 0, 800, 0], [3, -100, 500, 200]]);
  });

  it('maps back onto the scan by a pure translation by MAIN (no rotation, no mirror)', () => {
    const anchors = [MAIN, G1, G2, G3];
    const { layout } = buildTagLayout(anchors);
    expect(layout!.mainScanPositionMm).toEqual([1000, 1000, 1000]);
    for (const node of layout!.nodes) {
      const source = anchors.find((item) => item.tagId === node.tagId)!;
      const project = xrToProject(source.positionScan);
      expect(node.xMm + layout!.mainScanPositionMm[0]).toBeCloseTo(project[0] * 1000, 0);
      expect(node.yMm + layout!.mainScanPositionMm[1]).toBeCloseTo(project[1] * 1000, 0);
      expect(node.zMm + layout!.mainScanPositionMm[2]).toBeCloseTo(project[2] * 1000, 0);
    }
  });

  it('keeps the vertical from the scan: a tag 20 cm higher is +200 mm in Z, not a barometer estimate', () => {
    expect(buildTagLayout([MAIN, G1, G3]).layout!.nodes.find((n) => n.nodeId === 3)!.zMm).toBe(200);
  });

  it('propagates the position spread into the uncertainty relative to MAIN, and gives MAIN none', () => {
    const { layout } = buildTagLayout([anchor(0, 'MAIN', [1, 1, -1], true, 0.003), anchor(1, 'G1', [1.5, 1, -1], true, 0.004), G2]);
    expect(layout!.nodes[0].uncertaintyMm).toBe(0);
    expect(layout!.nodes[1].uncertaintyMm).toBe(5); // sqrt(4^2 + 3^2) mm
  });

  it('builds every triangle (more tags, more triangles) and reports the weakest one that includes MAIN', () => {
    expect(buildTagLayout([MAIN, G1, G2]).layout!.triangles).toHaveLength(1);
    const four = buildTagLayout([MAIN, G1, G2, G3]).layout!;
    expect(four.triangles).toHaveLength(4);
    expect(four.triangles.filter((t) => t.includesMain)).toHaveLength(3);
    expect(buildTagLayout([MAIN, G1, G2, G3, anchor(4, 'G4', [1.3, 1.05, -1.4])]).layout!.triangles).toHaveLength(10);
    const base = buildTagLayout([MAIN, G1, G2]).layout!;
    expect(base.triangles[0].heightMm).toBeCloseTo(triangleHeightMm([0, 0, 0], [500, 0, 0], [0, 800, 0]), 6);
    expect(base.weakestMainTriangleHeightMm).toBe(Math.round(base.triangles[0].heightMm));
    expect(four.weakestMainTriangleHeightMm).toBeLessThanOrEqual(base.weakestMainTriangleHeightMm);
  });

  it('needs MAIN and at least two other tags that are not in a line', () => {
    expect(buildTagLayout([G1, G2, G3])).toMatchObject({ layout: null, reason: 'no_main' });
    expect(buildTagLayout([MAIN, G1])).toMatchObject({ layout: null, reason: 'too_few_tags' });
    expect(buildTagLayout([])).toMatchObject({ layout: null, reason: 'no_main' });
    const collinear = buildTagLayout([MAIN, G1, anchor(2, 'G2', [2, 1, -1])]);
    expect(collinear).toMatchObject({ layout: null, reason: 'degenerate_triangles' });
    // The limit is the triangle's smallest altitude (30 mm): 10 mm off a 1 m line is only 5 mm tall, 70 mm off is 35 mm.
    expect(buildTagLayout([MAIN, G1, anchor(2, 'G2', [2, 1, -1.01])])).toMatchObject({ layout: null, reason: 'degenerate_triangles' });
    expect(buildTagLayout([MAIN, G1, anchor(2, 'G2', [2, 1, -1.07])]).layout).not.toBeNull();
  });

  it('takes a preliminary tag only if its position is tight enough; confirmed tags always join', () => {
    const loose = anchor(3, 'G3', [0.9, 1.2, -1.5], false, 0.2);
    const tight = anchor(3, 'G3', [0.9, 1.2, -1.5], false, 0.03);
    const a = buildTagLayout([MAIN, G1, G2, loose]);
    expect(a.layout!.nodes.map((n) => n.nodeId)).toEqual([0, 1, 2]);
    expect(a.excluded).toEqual([{ tagId: 3, node: 'G3', reason: 'provisional_spread_too_high' }]);
    const b = buildTagLayout([MAIN, G1, G2, tight]);
    expect(b.layout!.nodes.find((n) => n.nodeId === 3)).toMatchObject({ confirmed: false });
    const c = buildTagLayout([MAIN, G1, G2, anchor(3, 'G3', [0.9, 1.2, -1.5], true, 0.2)]);
    expect(c.layout!.nodes.map((n) => n.nodeId)).toEqual([0, 1, 2, 3]);
    expect(buildTagLayout([MAIN, G1, G2, loose], { provisionalMaxSpreadMm: 300 }).layout!.nodes).toHaveLength(4);
  });

  it('refuses two sensors in one place (a tag seen twice or two tags mixed up) and never drops MAIN for it', () => {
    const twin = anchor(2, 'G2', [1.5, 1, -1.004]); // 4 mm from G1
    const { layout, excluded } = buildTagLayout([MAIN, G1, twin, G3]);
    expect(layout!.nodes.map((n) => n.nodeId)).toEqual([0, 1, 3]);
    expect(excluded).toEqual([{ tagId: 2, node: 'G2', reason: 'too_close_to_another_tag' }]);
    const atMain = buildTagLayout([MAIN, anchor(1, 'G1', [1, 1, -1.002]), G2, G3]);
    expect(atMain.layout!.nodes.map((n) => n.nodeId)).toEqual([0, 2, 3]);
  });

  it('excludes unknown nodes and non-finite positions without failing the rest', () => {
    const { layout, excluded } = buildTagLayout([MAIN, G1, G2, anchor(9, 'G9', [0, 0, 0]), anchor(3, 'G3', [NaN, 1, 1])]);
    expect(layout!.nodes.map((n) => n.nodeId)).toEqual([0, 1, 2]);
    expect(excluded.map((e) => e.reason).sort()).toEqual(['invalid_position', 'unknown_node']);
  });

  it('keeps the confirmed reading when a node appears twice', () => {
    const { layout } = buildTagLayout([MAIN, G1, G2, anchor(2, 'G2', [1, 1, -1.9], false, 0.01), anchor(2, 'G2', [1, 1, -1.8], true, 0.003)]);
    expect(layout!.nodes.find((n) => n.nodeId === 2)).toMatchObject({ yMm: 800, confirmed: true });
  });
});

describe('comparison with an independent reference layout (Channel Sounding)', () => {
  const layout = buildTagLayout([MAIN, G1, G2, G3]).layout!;
  const rotate = (x: number, y: number, deg: number, mirror = false) => {
    const a = deg * Math.PI / 180; const px = mirror ? -x : x;
    return [px * Math.cos(a) - y * Math.sin(a), px * Math.sin(a) + y * Math.cos(a)];
  };
  const asReference = (deg: number, mirror: boolean) => layout.nodes.map((n) => { const [x, y] = rotate(n.xMm, n.yMm, deg, mirror); return { nodeId: n.nodeId, xMm: x, yMm: y, zMm: n.zMm }; });

  it('needs no common orientation: a rotated and mirrored copy agrees', () => {
    for (const [deg, mirror] of [[0, false], [70, false], [-130, true]] as const) {
      const agreement = compareLayoutWithReference(layout, asReference(deg, mirror));
      expect(agreement.pairs).toHaveLength(6);
      expect(Math.max(...agreement.pairs.map((p) => p.deltaMm))).toBeLessThan(1e-6);
      expect(agreement.suspectNodes).toEqual([]);
    }
  });

  it('flags a mixed-up tag: swapping G1 and G2 in the reference makes both nodes disagree', () => {
    const swapped = asReference(0, false).map((n) => (n.nodeId === 1 ? { ...n, nodeId: 2 } : n.nodeId === 2 ? { ...n, nodeId: 1 } : n));
    const agreement = compareLayoutWithReference(layout, swapped, 100);
    expect(agreement.suspectNodes).toContain(1);
    expect(agreement.suspectNodes).toContain(2);
  });

  it('reports but does not move the layout, ignores reference nodes it does not have, and honours the tolerance', () => {
    const before = JSON.stringify(layout);
    const noisy = asReference(20, false).map((n) => ({ ...n, xMm: n.xMm + (n.nodeId === 1 ? 600 : 0) }));
    const tight = compareLayoutWithReference(layout, noisy, 100);
    const loose = compareLayoutWithReference(layout, noisy, 5000);
    expect(tight.suspectNodes).toContain(1);
    expect(loose.suspectNodes).toEqual([]);
    expect(JSON.stringify(layout)).toBe(before);
    const partial = compareLayoutWithReference(layout, asReference(0, false).filter((n) => n.nodeId !== 3));
    expect(partial.pairs).toHaveLength(3);
    expect(compareLayoutWithReference(layout, [])).toMatchObject({ pairs: [], suspectNodes: [] });
  });
});

describe('tagLayoutReplaces', () => {
  const layoutOf = (confirmedFlags: boolean[]) => ({ nodes: confirmedFlags.map((confirmed) => ({ confirmed })) } as never);
  it('lets anything replace nothing, never lets nothing replace something', () => {
    expect(tagLayoutReplaces(layoutOf([true, true, true]), null)).toBe(true);
    expect(tagLayoutReplaces(null, null)).toBe(true);
    expect(tagLayoutReplaces(null, { nodes: 3, confirmed: 3 })).toBe(false);
  });
  it('lets a layout replace the saved one only when it has at least as many nodes and as many confirmed', () => {
    const saved = { nodes: 4, confirmed: 3 };
    expect(tagLayoutReplaces(layoutOf([true, true, true, false]), saved)).toBe(true);
    expect(tagLayoutReplaces(layoutOf([true, true, true, true, true]), saved)).toBe(true);
    expect(tagLayoutReplaces(layoutOf([true, true, true]), saved)).toBe(false);          // fewer nodes
    expect(tagLayoutReplaces(layoutOf([true, true, false, false]), saved)).toBe(false);  // fewer confirmed
  });
});

describe('tag anchors of a continuation extension move into the base frame (issue #37)', () => {
  const T_BA = { yaw: Math.PI / 2, tx: 3, ty: -2, tz: 0.5 };

  it('gives the layout the same B->A transform as the merged cloud', () => {
    const moved = tagAnchorsToBaseFrame([MAIN, G1, G2, G3], T_BA);
    const { layout } = buildTagLayout(moved);
    expect(layout!.mainScanPositionMm.map(Math.round)).toEqual([2000, -1000, 1500]);
    const byNode = new Map(layout!.nodes.map((n) => [n.nodeId, [Math.round(n.xMm) + 0, Math.round(n.yMm) + 0, Math.round(n.zMm) + 0]]));
    expect(byNode.get(1)).toEqual([0, 500, 0]);
    expect(byNode.get(2)).toEqual([-800, 0, 0]);
    expect(byNode.get(3)).toEqual([-500, -100, 200]);
  });

  it('keeps anchors untouched for a fresh scan and never mutates its input', () => {
    const before = JSON.stringify([MAIN, G1]);
    expect(tagAnchorsToBaseFrame([MAIN, G1], null)).toEqual([MAIN, G1]);
    tagAnchorsToBaseFrame([MAIN, G1], T_BA);
    expect(JSON.stringify([MAIN, G1])).toBe(before);
  });

  it('changes nothing about observation quality, only the position', () => {
    const [moved] = tagAnchorsToBaseFrame([G3], T_BA);
    expect({ ...moved, positionScan: null }).toEqual({ ...G3, positionScan: null });
  });
});
