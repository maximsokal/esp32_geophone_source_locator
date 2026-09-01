import { describe, expect, it } from 'vitest';

import {
  certifyCornerByVisualReprojection,
  certifyEdgeByVisualReprojection,
  certifyEndpointByVisualReprojection,
} from '../../src/xr/reprojectionCertificate';
import type { VisualEvidenceFrame } from '../../src/xr/visualLineEvidence';

const projection = new Float32Array([
  1, 0, 0, 0,
  0, 1, 0, 0,
  0, 0, 0, -1,
  0, 0, 0, 0,
]);

function projectedU(projectX: number, cameraX: number, projectY = 2): number {
  return 0.5 + (projectX - cameraX) / (2 * projectY);
}

function frame(keyframeId: number, cameraX: number): VisualEvidenceFrame {
  const left = projectedU(-0.5, cameraX);
  const right = projectedU(0.5, cameraX);
  const center = projectedU(0, cameraX);
  const worldFromView = new Float32Array([
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    cameraX, 0, 0, 1,
  ]);
  return {
    keyframeId,
    capturedAtMs: keyframeId * 100,
    projectionMatrix: projection.slice(),
    worldFromView,
    lines: [{
      id: `line:${keyframeId}`,
      start: [left, 0.5],
      end: [right, 0.5],
      direction: [1, 0],
      length: right - left,
      strength: 0.9,
      supportPixels: 40,
    }],
    junctions: [{
      position: [center, 0.5],
      strength: 0.88,
      lineAId: `a:${keyframeId}`,
      lineBId: `b:${keyframeId}`,
    }],
  };
}

describe('multi-view RGB reprojection certificates', () => {
  const independentFrames = [frame(1, 0), frame(2, 0.10), frame(3, 0.20)];

  it('certifies a 3D corner only after independent RGB junction reprojection', () => {
    const certificate = certifyCornerByVisualReprojection([0, 2, 0], independentFrames);
    expect(certificate.accepted).toBe(true);
    expect(certificate.conclusive).toBe(true);
    expect(certificate.eligibleObservations).toBe(3);
    expect(certificate.observations).toBe(3);
    expect(certificate.matchedKeyframes).toEqual([1, 2, 3]);
    expect(certificate.meanErrorNormalized).toBeLessThan(1e-5);
  });

  it('certifies finite edge endpoints from repeated RGB line endpoints', () => {
    const certificate = certifyEndpointByVisualReprojection([-0.5, 2, 0], independentFrames);
    expect(certificate.accepted).toBe(true);
    expect(certificate.conclusive).toBe(true);
    expect(certificate.observations).toBe(3);
    expect(certificate.confidence).toBeGreaterThan(0.7);
  });

  it('certifies a 3D edge against the same oriented 2D line in independent views', () => {
    const certificate = certifyEdgeByVisualReprojection([-0.5, 2, 0], [0.5, 2, 0], independentFrames);
    expect(certificate.accepted).toBe(true);
    expect(certificate.conclusive).toBe(true);
    expect(certificate.observations).toBe(3);
    expect(certificate.meanErrorNormalized).toBeLessThan(1e-5);
  });

  it('treats repeated evidence from essentially the same camera position as inconclusive instead of vetoing 3D geometry', () => {
    const repeated = [frame(1, 0), frame(2, 0.01), frame(3, 0.02), frame(4, 0.03)];
    const certificate = certifyCornerByVisualReprojection([0, 2, 0], repeated);
    expect(certificate.conclusive).toBe(false);
    expect(certificate.accepted).toBe(true);
    expect(certificate.eligibleObservations).toBe(1);
    expect(certificate.observations).toBe(1);
    expect(certificate.confidence).toBe(1);
  });

  it('does not let RGB structure elsewhere in the scene veto a candidate without enough candidate-specific views', () => {
    const certificate = certifyEdgeByVisualReprojection([20, 2, 0], [21, 2, 0], independentFrames);
    expect(certificate.eligibleObservations).toBe(0);
    expect(certificate.conclusive).toBe(false);
    expect(certificate.accepted).toBe(true);
  });

  it('rejects a visually inconsistent geometric corner once enough independent views actually observe that candidate', () => {
    const inconsistent = independentFrames.map((item) => ({
      ...item,
      junctions: item.junctions.map((junction) => ({
        ...junction,
        position: [junction.position[0] + 0.12, junction.position[1]] as const,
      })),
    }));
    const certificate = certifyCornerByVisualReprojection([0, 2, 0], inconsistent);
    expect(certificate.conclusive).toBe(true);
    expect(certificate.eligibleObservations).toBe(3);
    expect(certificate.accepted).toBe(false);
    expect(certificate.observations).toBe(0);
  });
});