import { describe, expect, it } from 'vitest';
import type { TagSensorAnchor } from '../../src/xr/fiducial/liveTagTracker';
import { buildTagLayout } from '../../src/xr/fiducial/tagLayout';
import {
  decodeTagPositions, encodeTagPositions, tagPositionsFromLayout, TAG_POSITIONS_MAX_BYTES, validateTagPositions, type TagPositionsArtifact,
} from '../../src/xr/fiducial/tagPositionsArtifact';

const anchor = (tagId: number, node: string, xr: [number, number, number], confirmed = true): TagSensorAnchor =>
  ({ tagId, node, positionScan: xr, spreadM: 0.003, observationCount: 8, independentViewCount: 4, confirmed });
const layout = buildTagLayout([anchor(0, 'MAIN', [1, 1, -1]), anchor(1, 'G1', [1.5, 1, -1]), anchor(2, 'G2', [1, 1, -1.8], false), anchor(3, 'G3', [0.9, 1.2, -1.5])]).layout!;
const valid = (): TagPositionsArtifact => tagPositionsFromLayout(layout, 'scan-crc32:732740d9', 1_700_000_000_000);
const mutate = (change: (a: any) => void): unknown => { const a = JSON.parse(JSON.stringify(valid())); change(a); return a; };

describe('tag-positions-v1.json', () => {
  it('carries the finished layout: three-axis integer millimetres per node, with the confirmed flag, spread and counts', () => {
    const a = valid();
    expect(a).toMatchObject({ schema: 1, kind: 'tag_positions', frame: 'sensor_main_origin_z_up_mm', scan_frame_id: 'scan-crc32:732740d9', main_scan_position_mm: [1000, 1000, 1000] });
    expect(a.nodes.map((n) => [n.node_id, n.tag_id, n.x_mm, n.y_mm, n.z_mm, n.confirmed])).toEqual([
      [0, 0, 0, 0, 0, true], [1, 1, 500, 0, 0, true], [2, 2, 0, 800, 0, false], [3, 3, -100, 500, 200, true],
    ]);
    expect(a.nodes[2]).toMatchObject({ observations: 8, views: 4, uncertainty_mm: 4 });
  });

  it('round-trips and stays far below the firmware size limit', () => {
    const text = encodeTagPositions(valid());
    expect(new TextEncoder().encode(text).byteLength).toBeLessThan(TAG_POSITIONS_MAX_BYTES / 2);
    expect(decodeTagPositions(text)).toEqual(valid());
    expect(encodeTagPositions({ ...valid(), scan_frame_id: null })).toContain('"scan_frame_id":null');
  });

  it.each([
    ['unknown root field', (a: any) => { a.extra = 1; }],
    ['unknown node field', (a: any) => { a.nodes[1].extra = 1; }],
    ['wrong schema', (a: any) => { a.schema = 2; }],
    ['wrong kind', (a: any) => { a.kind = 'registration'; }],
    ['wrong frame', (a: any) => { a.frame = 'webxr_local_floor'; }],
    ['bad scan id', (a: any) => { a.scan_frame_id = '../x'; }],
    ['fraction in a coordinate', (a: any) => { a.nodes[1].x_mm = 500.5; }],
    ['coordinate out of range', (a: any) => { a.nodes[1].x_mm = 300000; }],
    ['non-numeric coordinate', (a: any) => { a.nodes[1].y_mm = '5'; }],
    ['negative uncertainty', (a: any) => { a.nodes[1].uncertainty_mm = -1; }],
    ['non-boolean confirmed', (a: any) => { a.nodes[1].confirmed = 1; }],
    ['fewer than three nodes', (a: any) => { a.nodes = a.nodes.slice(0, 2); }],
    ['repeated node id', (a: any) => { a.nodes[2].node_id = 1; a.nodes[2].tag_id = 1; }],
    ['node id out of range', (a: any) => { a.nodes[3].node_id = 5; a.nodes[3].tag_id = 5; }],
    ['tag not the node\'s own', (a: any) => { a.nodes[1].tag_id = 3; }],
    ['no MAIN', (a: any) => { a.nodes = a.nodes.filter((n: any) => n.node_id !== 0); a.nodes.push({ ...a.nodes[0], node_id: 4, tag_id: 4, x_mm: 5000 }); }],
    ['MAIN not at the origin', (a: any) => { a.nodes[0].x_mm = 1; }],
    ['MAIN with uncertainty', (a: any) => { a.nodes[0].uncertainty_mm = 3; }],
    ['two nodes within 10 mm', (a: any) => { a.nodes[2].x_mm = 505; a.nodes[2].y_mm = 0; a.nodes[2].z_mm = 0; }],
    ['not an object', (a: any) => { Object.keys(a).forEach((k) => delete a[k]); }],
  ])('rejects %s', (_name, change) => {
    const artifact = mutate(change as (a: any) => void);
    expect(() => validateTagPositions(artifact)).toThrow();
    expect(() => encodeTagPositions(artifact as TagPositionsArtifact)).toThrow();
  });

  it('rejects arrays, null and oversize text on decode', () => {
    expect(() => validateTagPositions(null)).toThrow();
    expect(() => validateTagPositions([])).toThrow();
    expect(() => decodeTagPositions(`${JSON.stringify(valid())}${' '.repeat(TAG_POSITIONS_MAX_BYTES)}`)).toThrow(/too large/);
    expect(() => decodeTagPositions('{not json')).toThrow();
  });
});
