import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, expect, it } from 'vitest';

import {
  evaluateRelock,
  relockBaseTags,
  RELOCK_MAX_RMS_MM,
  type RelockBaseTag,
} from '../../src/xr/fiducial/continuationRelocalization';
import { baseToExtensionLocalFloor } from '../../src/xr/fiducial/referenceOverlay';
import { fuseTagObservations, type TagObservation } from '../../src/xr/fiducial/tagObservationFusion';
import { decodeTagPositions } from '../../src/xr/fiducial/tagPositionsArtifact';
import { decodeScanArtifact } from '../../src/xr/scanArtifact';
import { registerAndMergeScanContinuation } from '../../src/xr/scanContinuation';

/*
 * Replay of the real phone-lab run 2026-11-22 12:41 (OBJ_0001): the first scan A, saved with its tag layout, and the Continue B that the phone could not
 * merge (SCAN_CONTINUATION_REJECTED "alignment is ambiguous"; XR_RELOCK_UNAVAILABLE baseTags=1 because only G4 of the four usable tags was "confirmed").
 *
 * The fixtures are what the phone logged and saved: the XR_TAG_POSE samples of both sessions (WebXR local-floor metres, with the camera position) and
 * the 872-byte tag-positions-v1.json written for scan-crc32:26cb7bca. Nothing here is synthetic except the way the samples are fed to the fusion.
 */
const DIR = resolve(__dirname, '../fixtures/phone-lab-2026-11-22');
interface Pose { tagId: number; x: number; y: number; z: number; cam: [number, number, number]; t: number }
const poses = JSON.parse(readFileSync(resolve(DIR, 'tag-poses.json'), 'utf-8')) as { sessionA: Pose[]; sessionB: Pose[] };
const savedLayoutText = readFileSync(resolve(DIR, 'tag-positions-v1.json'), 'utf-8');
const SCAN_ID = 'scan-crc32:26cb7bca';

const observations = (samples: readonly Pose[]): TagObservation[] =>
  samples.map((pose) => ({ tagId: pose.tagId, positionScan: [pose.x, pose.y, pose.z] as const, cameraPosition: pose.cam, capturedAtMs: pose.t }));

describe('the real saved layout of scan A', () => {
  const layout = decodeTagPositions(savedLayoutText)!;

  it('is the layout the phone saved: five nodes, only G4 confirmed, the others seen 2-4 times from 1-3 views', () => {
    expect(layout.scan_frame_id).toBe(SCAN_ID);
    expect(layout.nodes.map((node) => [node.tag_id, node.confirmed, node.observations, node.views])).toEqual([
      [0, false, 4, 3], [1, false, 3, 2], [2, false, 4, 2], [3, false, 2, 1], [4, true, 6, 4],
    ]);
  });

  it('gives FOUR anchors, not one (the failure of this run: baseTags=1 left the Continue without any tie to A)', () => {
    const base = relockBaseTags(layout, SCAN_ID);
    expect(base.map((tag) => tag.tagId)).toEqual([0, 1, 2, 4]);          /* G3 (2 sightings, 1 view) stays out */
    const confirmedOnly = layout.nodes.filter((node) => node.confirmed);
    expect(confirmedOnly).toHaveLength(1);
  });
});

describe('the real tags of session B against the saved layout of A', () => {
  const base = relockBaseTags(decodeTagPositions(savedLayoutText)!, SCAN_ID);
  const fusionB = fuseTagObservations(observations(poses.sessionB), { minObservations: 3, minIndependentViews: 2 });

  it('locks B to A from the tags the phone actually saw, within the agreed gates', () => {
    const state = evaluateRelock(base, fusionB);
    expect(state.reason, JSON.stringify({ found: state.found, rms: state.rmsMm })).toBeNull();
    expect(state.phase).toBe('locked');
    expect(state.foundTagIds).toEqual([0, 1, 2, 4]);
    expect(state.usedTagIds.length).toBeGreaterThanOrEqual(3);
    expect(state.rmsMm!).toBeLessThanOrEqual(RELOCK_MAX_RMS_MM);
    expect(state.rmsMm!).toBeGreaterThan(0);
    /* What the real tags give (pinned so that a change of the gates or of the fit is noticed): yaw 44.8 degrees, MAIN-frame offset about (0.13, -0.86, 0) m. */
    expect(state.transform!.yaw).toBeCloseTo(0.781, 2);
    expect(state.transform!.tx).toBeCloseTo(0.130, 2);
    expect(state.transform!.ty).toBeCloseTo(-0.856, 2);
    expect(Math.abs(state.transform!.tz)).toBeLessThan(0.02);
    expect(state.usedTagIds).toEqual([0, 1, 2, 4]);
  });

  it('agrees with the independent geometric estimate the overlap search made in the same run, which it rejected as ambiguous', () => {
    /* SCAN_CONTINUATION_REJECTED diagnostics of this run: the best candidate of the point-cloud search (274 inliers, 30% overlap, RMS 8.3 cm). */
    const searchYaw = 0.81973859373965;
    const searchTranslation = [0.18463630080482807, -0.7937474261455264, -0.001764565938870477];
    const state = evaluateRelock(base, fusionB);
    const shift = Math.hypot(state.transform!.tx - searchTranslation[0], state.transform!.ty - searchTranslation[1], state.transform!.tz - searchTranslation[2]);
    expect(shift, 'metres between the tag lock and the overlap search').toBeLessThan(0.12);
    expect(Math.abs(state.transform!.yaw - searchYaw), 'radians between them').toBeLessThan(0.05);
  });

  it('the lock maps each saved tag of A onto where session B saw it (the overlay of A would sit on the real tags)', () => {
    const state = evaluateRelock(base, fusionB);
    expect(state.phase).toBe('locked');
    for (const tagId of state.usedTagIds) {
      const savedA = base.find((tag) => tag.tagId === tagId) as RelockBaseTag;
      const drawn = baseToExtensionLocalFloor([savedA.positionMm[0] / 1000, savedA.positionMm[1] / 1000, savedA.positionMm[2] / 1000], state.transform!);
      const seen = fusionB.byTagId[tagId as 0 | 1 | 2 | 3 | 4]!.positionScan;
      expect(Math.hypot(drawn[0] - seen[0], drawn[1] - seen[1], drawn[2] - seen[2]) * 1000, `tag ${tagId}`).toBeLessThan(40);
    }
  });

  it('is the same lock in any order the samples were logged', () => {
    const shuffled = [...poses.sessionB].reverse();
    const state = evaluateRelock(base, fuseTagObservations(observations(shuffled), { minObservations: 3, minIndependentViews: 2 }));
    const reference = evaluateRelock(base, fusionB);
    expect(state.phase).toBe('locked');
    expect(state.transform!.yaw).toBeCloseTo(reference.transform!.yaw, 9);
    expect(state.transform!.tx).toBeCloseTo(reference.transform!.tx, 9);
  });

  it('refuses to lock when two of the real tags are swapped (the guard against a mixed-up tag works on real numbers)', () => {
    const swapped = poses.sessionB.map((pose) => (pose.tagId === 1 ? { ...pose, tagId: 2 } : pose.tagId === 2 ? { ...pose, tagId: 1 } : pose));
    const state = evaluateRelock(base, fuseTagObservations(observations(swapped), { minObservations: 3, minIndependentViews: 2 }));
    expect(state.phase).toBe('relocalizing');
  });

  it('waits when B has seen fewer than three of the tags', () => {
    const two = poses.sessionB.filter((pose) => pose.tagId === 0 || pose.tagId === 1);
    const state = evaluateRelock(base, fuseTagObservations(observations(two), { minObservations: 3, minIndependentViews: 2 }));
    expect(state).toMatchObject({ phase: 'relocalizing', reason: 'need_more_tags' });
  });
});

describe('the real tags of session A against its own saved layout (the pipeline must give the identity)', () => {
  it('locks with a transform equal to the identity within a few millimetres', () => {
    const base = relockBaseTags(decodeTagPositions(savedLayoutText)!, SCAN_ID);
    const state = evaluateRelock(base, fuseTagObservations(observations(poses.sessionA), { minObservations: 3, minIndependentViews: 2 }));
    expect(state.phase).toBe('locked');
    expect(Math.abs(state.transform!.yaw)).toBeLessThan(0.02);
    expect(Math.hypot(state.transform!.tx, state.transform!.ty, state.transform!.tz) * 1000).toBeLessThan(20);
  });
});

/*
 * The real point cloud of scan A (3,249 stable points), which is too large for the repository: read from the phone-lab saves when they are present, skipped
 * otherwise, never faked. B is made from A itself: the half of A that a second pass would see again, taken into session B's frame by the INVERSE of the
 * lock the real tags gave and jittered by 8 mm, so the only unknown in the merge is the registration.
 */
const SAVES = resolve(__dirname, '../../../logs/phone-lab/2026-11-22_124115/saves');
describe.skipIf(!existsSync(SAVES))('the real cloud of scan A merged through the tag lock (the Continue the phone could not merge)', () => {
  const scanFile = (): Uint8Array => new Uint8Array(readFileSync(resolve(SAVES, readdirSync(SAVES).find((entry) => entry.includes('_scan_'))!)));
  const lock = (() => {
    const base = relockBaseTags(decodeTagPositions(savedLayoutText)!, SCAN_ID);
    return evaluateRelock(base, fuseTagObservations(observations(poses.sessionB), { minObservations: 3, minIndependentViews: 2 })).transform!;
  })();

  function extensionFrom(base: ReturnType<typeof decodeScanArtifact>) {
    const c = Math.cos(-lock.yaw); const s = Math.sin(-lock.yaw);
    const keep: number[] = [];
    const sorted = Array.from({ length: base.pointCount }, (_, i) => base.positions[i * 3]).sort((a, b) => a - b);
    const median = sorted[Math.floor(sorted.length / 2)];
    for (let i = 0; i < base.pointCount; i += 1) if (base.positions[i * 3] >= median - 0.3) keep.push(i);   /* a bit more than half: the overlap plus new ground */
    const positions = new Float32Array(keep.length * 3);
    const normals = new Float32Array(keep.length * 3);
    const confidence = new Float32Array(keep.length);
    const normalConfidence = new Float32Array(keep.length);
    let jitter = 12345;
    const noise = (): number => { jitter = (jitter * 1664525 + 1013904223) >>> 0; return ((jitter / 0xffffffff) - 0.5) * 0.016; };
    keep.forEach((index, row) => {
      const x = base.positions[index * 3] - lock.tx; const y = base.positions[index * 3 + 1] - lock.ty; const z = base.positions[index * 3 + 2] - lock.tz;
      positions[row * 3] = c * x - s * y + noise(); positions[row * 3 + 1] = s * x + c * y + noise(); positions[row * 3 + 2] = z + noise();
      const nx = base.normals[index * 3]; const ny = base.normals[index * 3 + 1];
      normals[row * 3] = c * nx - s * ny; normals[row * 3 + 1] = s * nx + c * ny; normals[row * 3 + 2] = base.normals[index * 3 + 2];
      confidence[row] = base.confidence[index]; normalConfidence[row] = base.normalConfidence[index];
    });
    return { ...base, positions, normals, confidence, normalConfidence, pointCount: keep.length };
  }

  it('merges: the seeded registration accepts the real A and a SYNTHETIC extension derived from it (the real B cloud of that run was never saved, so this proves the registration path, not the phone capture)', () => {
    const base = decodeScanArtifact(scanFile());
    const extension = extensionFrom(base);
    expect(base.pointCount).toBe(3249);
    const seeded = registerAndMergeScanContinuation(base, extension, undefined, lock);
    expect(seeded.ok, seeded.ok ? '' : `${seeded.reason} ${JSON.stringify(seeded.diagnostics)}`).toBe(true);
    if (!seeded.ok) return;
    expect(seeded.diagnostics.inliers).toBeGreaterThan(100);
    expect(seeded.diagnostics.rmsMeters).toBeLessThan(0.05);
    expect(seeded.diagnostics.runtimeOperations?.yawHypotheses).toBe(0);
    expect(Math.hypot(seeded.extensionToBase.tx - lock.tx, seeded.extensionToBase.ty - lock.ty, seeded.extensionToBase.tz - lock.tz)).toBeLessThan(0.1);
    expect(Math.abs(seeded.extensionToBase.yaw - lock.yaw)).toBeLessThan(0.05);
    expect(seeded.snapshot.pointCount).toBeGreaterThanOrEqual(base.pointCount);                       /* the merge never loses what A had */
  });
});
