import { describe, expect, it } from 'vitest';
import {
  WebXrFusionKeyframeSelector,
  type FusionPoseLike,
} from '../../src/xr/webxrFusionQuality';

function pose(
  x = 0,
  y = 0,
  z = 0,
  yawDegrees = 0,
): FusionPoseLike {
  const half = (yawDegrees * Math.PI / 180) * 0.5;
  return {
    position: { x, y, z },
    orientation: { x: 0, y: Math.sin(half), z: 0, w: Math.cos(half) },
  };
}

describe('WebXrFusionKeyframeSelector', () => {
  it('accepts the first valid pose as the initial keyframe', () => {
    const selector = new WebXrFusionKeyframeSelector();
    const result = selector.evaluate(0, pose());
    expect(result.kind).toBe('keyframe');
    expect(result.keyframeId).toBe(1);
    expect(result.motionQuality).toBe(1);
  });

  it('rejects invalid quaternion input without advancing evidence', () => {
    const selector = new WebXrFusionKeyframeSelector();
    const invalid: FusionPoseLike = {
      position: { x: 0, y: 0, z: 0 },
      orientation: { x: 0, y: 0, z: 0, w: 0 },
    };
    expect(selector.evaluate(0, invalid).kind).toBe('skip');
    expect(selector.evaluate(100, pose()).keyframeId).toBe(1);
  });

  it('does not create a new keyframe while stationary', () => {
    const selector = new WebXrFusionKeyframeSelector();
    selector.evaluate(0, pose());
    expect(selector.evaluate(300, pose()).kind).toBe('skip');
  });

  it('emits low-weight polish after the stationary polish interval', () => {
    const selector = new WebXrFusionKeyframeSelector();
    selector.evaluate(0, pose());
    const result = selector.evaluate(500, pose());
    expect(result.kind).toBe('polish');
    expect(result.state).toBe('stationary');
    expect(result.frameWeight).toBeLessThan(0.25);
  });

  it('creates a new keyframe after sufficient translation and time', () => {
    const selector = new WebXrFusionKeyframeSelector();
    selector.evaluate(0, pose());
    selector.evaluate(150, pose(0.02));
    const result = selector.evaluate(320, pose(0.07));
    expect(result.kind).toBe('keyframe');
    expect(result.keyframeId).toBe(2);
  });

  it('does not create a keyframe before the minimum keyframe interval', () => {
    const selector = new WebXrFusionKeyframeSelector();
    selector.evaluate(0, pose());
    const result = selector.evaluate(100, pose(0.08));
    expect(result.kind).not.toBe('keyframe');
  });

  it('requires some translation for a rotation-driven keyframe', () => {
    const selector = new WebXrFusionKeyframeSelector();
    selector.evaluate(0, pose());
    const rotationOnly = selector.evaluate(300, pose(0, 0, 0, 18));
    expect(rotationOnly.kind).not.toBe('keyframe');
  });

  it('accepts a combined translation + rotation viewpoint', () => {
    const selector = new WebXrFusionKeyframeSelector();
    selector.evaluate(0, pose());
    const result = selector.evaluate(320, pose(0.04, 0, 0, 15));
    expect(result.kind).toBe('keyframe');
  });

  it('treats q and -q as the same physical orientation', () => {
    const selector = new WebXrFusionKeyframeSelector();
    selector.evaluate(0, pose());
    const samePoseOppositeQuaternion: FusionPoseLike = {
      position: { x: 0, y: 0, z: 0 },
      orientation: { x: 0, y: 0, z: 0, w: -1 },
    };
    const result = selector.evaluate(500, samePoseOppositeQuaternion);
    expect(result.angularSpeedRadiansPerSecond).toBeCloseTo(0, 6);
  });

  it('rejects a violent translation as too-fast rather than map evidence', () => {
    const selector = new WebXrFusionKeyframeSelector();
    selector.evaluate(0, pose());
    const result = selector.evaluate(20, pose(0.5));
    expect(result.kind).toBe('skip');
    expect(result.state).toBe('too-fast');
    expect(result.motionQuality).toBeLessThan(0.25);
  });

  it('reports linear acceleration from changing pose velocity', () => {
    const selector = new WebXrFusionKeyframeSelector();
    selector.evaluate(0, pose());
    selector.evaluate(100, pose(0.005));
    const result = selector.evaluate(200, pose(0.04));
    expect(result.linearAccelerationMetersPerSecond2).toBeGreaterThan(0);
  });

  it('reports angular speed for changing orientation', () => {
    const selector = new WebXrFusionKeyframeSelector();
    selector.evaluate(0, pose());
    const result = selector.evaluate(200, pose(0, 0, 0, 8));
    expect(result.angularSpeedRadiansPerSecond).toBeGreaterThan(0);
  });

  it('reset makes the next valid sample a fresh keyframe 1', () => {
    const selector = new WebXrFusionKeyframeSelector();
    selector.evaluate(0, pose());
    selector.evaluate(400, pose(0.1));
    selector.reset();
    const result = selector.evaluate(1000, pose(2));
    expect(result.kind).toBe('keyframe');
    expect(result.keyframeId).toBe(1);
  });
});
