import { describe, expect, it } from 'vitest';
import { tagSensorAnchorsFromFusion, type TagSensorAnchor } from '../../src/xr/fiducial/liveTagTracker';
import { fuseTagObservations, type TagObservation } from '../../src/xr/fiducial/tagObservationFusion';
import { mergeLiveTagAnchorMarkers, type SensorPlacementMarker } from '../../src/xr/sensorPlacementScene';

const anchor = (tagId: number, node: string, position: [number, number, number] = [0.5, 1.0, -0.4]): TagSensorAnchor =>
  ({ tagId, node, positionScan: position, spreadM: 0.004, observationCount: 9, independentViewCount: 4 });

const registered = (node: SensorPlacementMarker['node']): SensorPlacementMarker => ({
  node, status: 'anchored', source: 'apriltag', positionScanM: [1, 2, 3], uncertaintyM: 0.01, residualM: 0.002, tagId: 1, details: 'registered',
});
const unplaced = (node: SensorPlacementMarker['node']): SensorPlacementMarker => ({
  node, status: 'cs_unregistered', source: 'channel_sounding_baro', positionScanM: null, uncertaintyM: 0.3, residualM: null, tagId: null, details: 'cs only',
});

describe('live tag anchors on the result view', () => {
  it('adds a visual_only AprilTag marker in the project frame (X = XR X, Y = -XR Z, Z = XR Y)', () => {
    const merged = mergeLiveTagAnchorMarkers([], [anchor(2, 'G2', [0.5, 1.0, -0.4])]);
    expect(merged).toHaveLength(1);
    expect(merged[0]).toMatchObject({ node: 'G2', status: 'visual_only', source: 'apriltag', tagId: 2, uncertaintyM: 0.004, positionScanM: [0.5, 0.4, 1.0] });
    expect(merged[0].details).toContain('9 sightings from 4 viewpoints');
  });

  it('never replaces or removes what the scene already holds for a node', () => {
    const scene = [registered('G1'), unplaced('G2')];
    const merged = mergeLiveTagAnchorMarkers(scene, [anchor(1, 'G1'), anchor(2, 'G2'), anchor(3, 'G3')]);
    expect(merged.filter((marker) => marker.node === 'G1')).toEqual([registered('G1')]); // a positioned marker keeps the node
    const g2 = merged.filter((marker) => marker.node === 'G2');
    expect(g2).toHaveLength(2); // the channel-sounding information stays, the tag position stands beside it
    expect(g2[0]).toEqual(unplaced('G2'));
    expect(g2[1]).toMatchObject({ status: 'visual_only', source: 'apriltag' });
    expect(merged.find((marker) => marker.node === 'G3')!.status).toBe('visual_only');
    expect(scene).toEqual([registered('G1'), unplaced('G2')]); // the input is not mutated
  });

  it('ignores non-finite positions and unknown nodes', () => {
    expect(mergeLiveTagAnchorMarkers([], [anchor(2, 'G2', [NaN, 1, 1]), anchor(9, 'G9'), anchor(0, 'MAIN')]).map((marker) => marker.node)).toEqual(['MAIN']);
  });

  it('turns only tags confirmed from several viewpoints into anchors', () => {
    const obs = (tagId: number, i: number): TagObservation => ({
      tagId, positionScan: [tagId, 1, -0.5], capturedAtMs: i, cameraPosition: [i * 0.4, 1.3, 0.5], cameraForward: [0, -0.2, -1],
    });
    const observations = [...Array.from({ length: 6 }, (_, i) => obs(0, i)), ...Array.from({ length: 2 }, (_, i) => obs(2, i))];
    const fusion = fuseTagObservations(observations, { minObservations: 5, minIndependentViews: 3 });
    expect(fusion.byTagId[2]).toBeDefined();
    expect(fusion.byTagId[2]!.confirmed).toBe(false);
    const anchors = tagSensorAnchorsFromFusion(fusion);
    expect(anchors.map((a) => a.node)).toEqual(['MAIN']);
    expect(anchors[0]).toMatchObject({ tagId: 0, observationCount: 6 });
  });
});
