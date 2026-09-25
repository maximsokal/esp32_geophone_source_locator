import { describe, expect, it } from 'vitest';

import { DepthPointCloudAccumulator } from '../../src/xr/depthPointCloudV2';
import { StructuralGuidePipeline } from '../../src/xr/structuralGuidePipeline';
import {
  ARCORE_NOISE, boxCornerScene, nearestPolygon, orbitPoses, percentile, rectangle, renderDepth, ridgeScene,
  scanScene, sceneNormalErrorDeg, tiltedPlaneScene, type CameraPose, type SyntheticScene,
} from './support/syntheticDepthScene';

/*
 * Phone power budget: once a coverage cell is ready, flat or gently bent, and
 * every surfel in it has matured, extra samples only re-confirm known geometry
 * (the point set is already at its plateau). Such settled cells keep a quarter
 * of the samples on the expensive path. Features, maturing surfels and newly
 * appearing geometry must keep full sampling.
 */

function longOrbit(scene: SyntheticScene): CameraPose[] {
  return [
    ...orbitPoses(scene.focus, 1.3, 60, 0, 330, 12),
    ...orbitPoses(scene.focus, 1.1, 55, 15, 345, 12),
    ...orbitPoses(scene.focus, 1.2, 65, 5, 335, 12),
  ];
}

/** Share of base samples skipped over the last third of a long scan, plus the final cloud. */
function run(scene: SyntheticScene) {
  const accumulator = new DepthPointCloudAccumulator(0.04);
  const poses = longOrbit(scene);
  let skipped = 0; let processed = 0;
  poses.forEach((pose, index) => {
    const frame = renderDepth(scene, pose, index + 1, ARCORE_NOISE);
    const result = accumulator.sampleDepthFrame(frame.depth, frame.view, { keyframeId: index + 1, evidenceKeyframe: true, frameWeight: 1, cameraProjectPosition: pose.eye });
    if (index >= 24) { skipped += result.settledSkippedSamples; processed += result.acceptedSamples; }
  });
  return { accumulator, skippedShare: skipped / Math.max(1, skipped + processed), snapshot: accumulator.snapshot(poses.length, poses.length) };
}

describe('settled flat regions stop costing full sampling', () => {
  for (const [label, scene] of [['flat plane', tiltedPlaneScene(20, 0, 1.2)], ['gently bent (170 deg) surface', ridgeScene(170, [0, 0, 0], 1.2, 1.2)]] as const) {
    it(`${label}: most late samples skip the expensive path, geometry stays within sensor precision`, () => {
      const { skippedShare, snapshot } = run(scene);
      // Measured ~0.67-0.69; half is the requirement.
      expect(skippedShare).toBeGreaterThanOrEqual(0.5);
      const distances: number[] = []; const normalErrors: number[] = [];
      for (let i = 0; i < snapshot.pointCount; i += 1) {
        const p = [snapshot.positions[i * 3], snapshot.positions[i * 3 + 1], snapshot.positions[i * 3 + 2]] as const;
        distances.push(nearestPolygon(scene, p).distance);
        normalErrors.push(sceneNormalErrorDeg(scene, p, [snapshot.normals[i * 3], snapshot.normals[i * 3 + 1], snapshot.normals[i * 3 + 2]]));
      }
      // Depth noise at 1.1-1.3 m is ~2-3 mm (1 sigma); fused surfels must stay well inside it.
      expect(distances.reduce((sum, d) => sum + d, 0) / distances.length).toBeLessThan(0.0015);
      expect(percentile(normalErrors, 0.9)).toBeLessThanOrEqual(4);
    });
  }

  it('structured geometry keeps (most of) its full sampling', () => {
    const flat = run(tiltedPlaneScene(20, 0, 1.2)).skippedShare;
    const corner = run(boxCornerScene()).skippedShare;
    expect(corner).toBeLessThan(flat / 2);
  });

  it('new geometry appearing in a settled region is still captured', () => {
    const floor = tiltedPlaneScene(0, 0, 1.2);
    const withBox: SyntheticScene = {
      ...floor, name: 'floor+box',
      polygons: [
        ...floor.polygons,
        rectangle([0.1, 0.1, 0.15], [0.1, 0, 0], [0, 0.1, 0], 'boxTop'),
        rectangle([0.2, 0.1, 0.075], [0, 0.1, 0], [0, 0, 0.075], 'box+x'),
        rectangle([0.1, 0.2, 0.075], [0, 0, 0.075], [0.1, 0, 0], 'box+y'),
        rectangle([0.0, 0.1, 0.075], [0, 0, 0.075], [0, 0.1, 0], 'box-x'),
        rectangle([0.1, 0.0, 0.075], [0.1, 0, 0], [0, 0, 0.075], 'box-y'),
      ],
    };
    const accumulator = new DepthPointCloudAccumulator(0.04);
    scanScene(accumulator, floor, [...orbitPoses([0, 0, 0], 1.2, 60, 0, 330, 12), ...orbitPoses([0, 0, 0], 1.0, 55, 15, 345, 12)], { noise: ARCORE_NOISE });
    scanScene(accumulator, withBox, orbitPoses([0.1, 0.1, 0.1], 1.0, 55, 0, 315, 10), { noise: ARCORE_NOISE, firstKeyframeId: 25 });
    const s = accumulator.snapshot(34, 34);
    let top = 0;
    for (let i = 0; i < s.pointCount; i += 1) {
      if (Math.abs(s.positions[i * 3 + 2] - 0.15) < 0.01 && Math.abs(s.positions[i * 3] - 0.1) < 0.1 && Math.abs(s.positions[i * 3 + 1] - 0.1) < 0.1) top += 1;
    }
    // 0.04 m^2 top x ~400 surfels/m^2 = ~16 expected; require 60 %.
    expect(top).toBeGreaterThanOrEqual(10);
  }, 30_000);
});

describe('structural pipeline hysteresis', () => {
  it('does not blink tracked edges off when readiness dips for a keyframe', () => {
    const scene = ridgeScene(90);
    const accumulator = new DepthPointCloudAccumulator(0.04);
    const pipeline = new StructuralGuidePipeline();
    const edgesPerFrame: number[] = [];
    orbitPoses(scene.focus, 0.8, 55, 0, 315, 16).forEach((pose, index) => {
      const frame = renderDepth(scene, pose, index + 1, ARCORE_NOISE);
      accumulator.sampleDepthFrame(frame.depth, frame.view, { keyframeId: index + 1, evidenceKeyframe: true, frameWeight: 1, cameraProjectPosition: pose.eye });
      const g = accumulator.structuralGuideSnapshot();
      const { result } = pipeline.analyze({ type: 'analyze', requestId: index, epoch: 0, positions: g.positions, normals: g.normals, confidence: g.confidence, keyframeCount: g.keyframeCount, voxelSizeMeters: g.voxelSizeMeters, viewpoint: { position: pose.eye, keyframeId: index + 1 } });
      edgesPerFrame.push(result.edges.length);
    });
    const firstStable = edgesPerFrame.findIndex((count, i) => count > 0 && i >= 1 && edgesPerFrame[i - 1] > 0);
    expect(firstStable).toBeGreaterThanOrEqual(0);
    for (let i = firstStable; i < edgesPerFrame.length; i += 1) expect(edgesPerFrame[i], `frame ${i + 1}: ${edgesPerFrame.join(',')}`).toBeGreaterThan(0);
  }, 30_000);
});
