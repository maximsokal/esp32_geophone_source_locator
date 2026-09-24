import { describe, expect, it } from 'vitest';

import { DepthPointCloudAccumulator } from '../../src/xr/depthPointCloudV2';
import { StructuralGuidePipeline } from '../../src/xr/structuralGuidePipeline';
import { detectCertifiedStructuralCorners } from '../../src/xr/certifiedStructuralGeometry';
import { prepareStructuralGuideReadiness } from '../../src/xr/structuralGuideReadiness';
import {
  ARCORE_NOISE, boxCornerScene, insideCornerScene, orbitPoses, renderDepth, ridgeScene, scanScene, tiltedPlaneScene,
  type CameraPose, type SyntheticScene, type Vec3,
} from './support/syntheticDepthScene';

/*
 * MAX-259: the production pipeline must certify concave (inside) corners as
 * well as convex ones, and still never invent a corner with fewer than three
 * faces. Everything runs through StructuralGuidePipeline, as the worker does.
 */

function run(scene: SyntheticScene, poses: readonly CameraPose[]) {
  const accumulator = new DepthPointCloudAccumulator(0.04);
  const pipeline = new StructuralGuidePipeline();
  let last: ReturnType<StructuralGuidePipeline['analyze']> | null = null;
  poses.forEach((pose, index) => {
    const frame = renderDepth(scene, pose, index + 1, ARCORE_NOISE);
    accumulator.sampleDepthFrame(frame.depth, frame.view, { keyframeId: index + 1, evidenceKeyframe: true, frameWeight: 1, cameraProjectPosition: pose.eye });
    const g = accumulator.structuralGuideSnapshot();
    last = pipeline.analyze({ type: 'analyze', requestId: index, epoch: 0, positions: g.positions, normals: g.normals, confidence: g.confidence, keyframeCount: g.keyframeCount, voxelSizeMeters: g.voxelSizeMeters, viewpoint: { position: pose.eye, keyframeId: index + 1 } });
  });
  return last!.result;
}

function nearest(corners: ReadonlyArray<{ position: readonly [number, number, number] }>, truth: Vec3): number {
  return Math.min(Infinity, ...corners.map((c) => Math.hypot(c.position[0] - truth[0], c.position[1] - truth[1], c.position[2] - truth[2])));
}

describe('certified corner seeding', () => {
  it('certifies a concave corner even when every seed lies >= 10 cm from the vertex', () => {
    // 16-pose scan: the highest-confidence point of every seed cell near this
    // inside corner is >= 10 cm away (occluded vertex) and a solve from there
    // only locks an edge; the three-plane hypothesis must recover the corner.
    const scene = insideCornerScene();
    const accumulator = new DepthPointCloudAccumulator(0.04);
    scanScene(accumulator, scene, orbitPoses(scene.focus, 1.2, 40, 10, 80, 16), { noise: ARCORE_NOISE });
    const s = prepareStructuralGuideReadiness(accumulator.structuralGuideSnapshot()).structuralSurface;
    const corners = detectCertifiedStructuralCorners({
      positions: s.positions, normals: s.normals, confidence: s.confidence, residualMeters: s.residualMeters,
      normalConfidence: s.normalConfidence, coverageConfidence: s.coverageConfidence, coverageReady: s.coverageReady,
      pointCount: s.pointCount, voxelSizeMeters: s.voxelSizeMeters,
    });
    expect(nearest(corners, scene.corners[0])).toBeLessThanOrEqual(0.02);
  });
});

describe('certified corners in the production pipeline', () => {
  it('certifies a concave inside corner within 2 cm', () => {
    // Occluded surfels at a concave vertex mature slowly: the solver locks the
    // corner only once its third face is well supported, and the tracker then
    // needs two independent observations. 32 poses over the same 70 deg arc.
    const scene = insideCornerScene();
    const result = run(scene, orbitPoses(scene.focus, 1.2, 40, 10, 80, 32));
    expect(nearest(result.corners, scene.corners[0])).toBeLessThanOrEqual(0.02);
  });

  it('still certifies a convex box corner within 2 cm', () => {
    const scene = boxCornerScene();
    const result = run(scene, orbitPoses(scene.focus, 1.0, 35, 10, 80, 16));
    expect(nearest(result.corners, scene.corners[0])).toBeLessThanOrEqual(0.02);
  });

  it('negative control: a two-face ridge yields edges but no corner', () => {
    const scene = ridgeScene(90);
    const result = run(scene, orbitPoses(scene.focus, 0.8, 55, 0, 315, 16));
    expect(result.edges.length).toBeGreaterThan(0);
    expect(result.corners).toHaveLength(0);
  });

  it('negative control: a flat floor yields no corner', () => {
    const scene = tiltedPlaneScene(0, 0, 1.2);
    const result = run(scene, orbitPoses(scene.focus, 1.2, 60, 0, 330, 16));
    expect(result.corners).toHaveLength(0);
  });
});
