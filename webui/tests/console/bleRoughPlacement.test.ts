import { describe, expect, it } from 'vitest';

import {
  BleRoughPlacement, HISTORY_LIMIT, MIN_ROUGH_RADIUS_M, RSSI_AT_1M_DBM, STALE_AFTER_MS,
  offscreenIndicator, rssiDistanceInterval, type Vec3,
} from '../../src/xr/bleRoughPlacement';

/* MAX-146: RSSI gives only rough, honestly-uncertain placement. */

function modelRssi(distanceM: number, noiseDb: number): number {
  return RSSI_AT_1M_DBM - 10 * 2.7 * Math.log10(distanceM) + noiseDb;
}

/* Deterministic +-3 dB "fading". */
const NOISE = [2.1, -1.4, 0.3, -2.8, 1.7, -0.6, 2.9, -2.2, 0.9, -1.1, 1.3, -2.5];

function walk(placement: BleRoughPlacement, nodeId: number, node: Vec3, path: Vec3[], t0 = 0): void {
  path.forEach((phone, i) => {
    const d = Math.hypot(node[0] - phone[0], node[1] - phone[1], node[2] - phone[2]);
    expect(placement.addSample({
      nodeId, rssiDbm: modelRssi(d, NOISE[i % NOISE.length]), txPowerDbm: null, phonePosition: phone, timestampMs: t0 + i * 500,
    })).toBe(true);
  });
}

const square: Vec3[] = [];
for (let i = 0; i < 24; i += 1) {
  const a = (i / 24) * 2 * Math.PI;
  square.push([1.5 * Math.cos(a), 1.4, 1.5 * Math.sin(a)]);
}

describe('rssiDistanceInterval', () => {
  it('is wide, ordered and rejects impossible readings', () => {
    const d = rssiDistanceInterval(-75)!;
    expect(d.minM).toBeLessThan(d.medianM);
    expect(d.maxM).toBeGreaterThan(d.medianM);
    expect(d.maxM / d.minM).toBeGreaterThan(3);
    expect(rssiDistanceInterval(RSSI_AT_1M_DBM)!.medianM).toBeCloseTo(1, 6);
    expect(rssiDistanceInterval(5)).toBeNull();
    expect(rssiDistanceInterval(Number.NaN)).toBeNull();
    expect(rssiDistanceInterval(-200)).toBeNull();
    /* Stronger advertised TX power -> same RSSI means farther away. */
    expect(rssiDistanceInterval(-75, 8)!.medianM).toBeGreaterThan(d.medianM);
  });
});

describe('BleRoughPlacement', () => {
  it('only shows a distance ring while the phone has not moved enough', () => {
    const placement = new BleRoughPlacement();
    walk(placement, 2, [4, 0, 2], Array.from({ length: 10 }, () => [0, 1.4, 0] as Vec3));
    const e = placement.estimate(2, 5000);
    expect(e.state).toBe('ble_detected');
    expect(e.label).toBe('REMOTE 2');
    expect(e.centre).toEqual([0, 1.4, 0]);
    expect(e.uncertaintyM).toBeNull();
    expect(e.ringM!.minM).toBeLessThan(4.5);
    expect(e.ringM!.maxM).toBeGreaterThan(4.5);
    expect(e.confidence).toBeLessThanOrEqual(0.1);
  });

  it('gives a rough horizontal blob that contains the true node after walking around', () => {
    const placement = new BleRoughPlacement();
    const node: Vec3 = [4, 0, 2];
    walk(placement, 1, node, square);
    const e = placement.estimate(1, 12_000);
    expect(e.state).toBe('rough');
    expect(e.uncertaintyM!).toBeGreaterThanOrEqual(MIN_ROUGH_RADIUS_M);
    const miss = Math.hypot(e.centre![0] - node[0], e.centre![2] - node[2]);
    expect(miss).toBeLessThanOrEqual(e.uncertaintyM!);
    /* Useful, not a trivially huge blob: bearing from the walk centre is right. */
    expect(e.uncertaintyM!).toBeLessThan(6);
    const bearingError = Math.abs(Math.atan2(e.centre![2], e.centre![0]) - Math.atan2(node[2], node[0]));
    expect(bearingError).toBeLessThan(Math.PI / 6);
    /* Never claims to be as good as Channel Sounding or a visual anchor. */
    expect(e.confidence).toBeGreaterThan(0);
    expect(e.confidence).toBeLessThanOrEqual(0.4);
    /* Height is the phone track's, not an invented node height. */
    expect(e.centre![1]).toBeCloseTo(1.4, 6);
  });

  it('lets a visual anchor replace RSSI and keeps RSSI as diagnostics only', () => {
    const placement = new BleRoughPlacement();
    walk(placement, 3, [2, 0, -3], square);
    placement.setVisualAnchor(3, [2.05, 0.02, -2.97]);
    const e = placement.estimate(3, 12_000);
    expect(e.state).toBe('anchored');
    expect(e.centre).toEqual([2.05, 0.02, -2.97]);
    expect(e.confidence).toBe(1);
    const diag = placement.diagnostics(12_000).find((d) => d.nodeId === 3)!;
    expect(diag.state).toBe('anchored');
    expect(diag.sampleCount).toBe(square.length);
    expect(diag.rssiHistory).toHaveLength(square.length);
    placement.setVisualAnchor(3, null);
    expect(placement.estimate(3, 12_000).state).toBe('rough');
  });

  it('drops everything on reset because a new XR session has a new frame', () => {
    const placement = new BleRoughPlacement();
    walk(placement, 1, [4, 0, 2], square);
    placement.setVisualAnchor(2, [1, 0, 1]);
    placement.reset();
    expect(placement.estimates(12_000).every((e) => e.state === 'not_detected' && e.sampleCount === 0)).toBe(true);
  });

  it('forgets stale RSSI, bounds history and rejects unknown nodes', () => {
    const placement = new BleRoughPlacement();
    walk(placement, 0, [1, 0, 1], square);
    expect(placement.estimate(0, 12_000).label).toBe('MAIN');
    expect(placement.estimate(0, 11_500 + STALE_AFTER_MS + 1).state).toBe('not_detected');
    expect(placement.estimate(4, 0).state).toBe('not_detected');
    for (let i = 0; i < HISTORY_LIMIT + 10; i += 1) {
      placement.addSample({ nodeId: 4, rssiDbm: -70, txPowerDbm: null, phonePosition: [0, 0, 0], timestampMs: i });
    }
    expect(placement.estimate(4, HISTORY_LIMIT + 10).sampleCount).toBe(HISTORY_LIMIT);
    expect(placement.addSample({ nodeId: 9, rssiDbm: -70, txPowerDbm: null, phonePosition: [0, 0, 0], timestampMs: 0 })).toBe(false);
    expect(placement.addSample({ nodeId: 1, rssiDbm: -70, txPowerDbm: null, phonePosition: [0, Number.NaN, 0], timestampMs: 0 })).toBe(false);
    expect(placement.estimates(0)).toHaveLength(5);
  });
});

describe('offscreenIndicator', () => {
  const identity = { position: [0, 0, 0] as Vec3, orientation: [0, 0, 0, 1] as const };

  it('is on screen straight ahead and off screen to the side or behind', () => {
    expect(offscreenIndicator(identity, [0, 0, -5]).visible).toBe(true);
    const right = offscreenIndicator(identity, [5, 0, 0]);
    expect(right.visible).toBe(false);
    expect(right.angleRad).toBeCloseTo(0, 6);
    expect(right.distanceM).toBeCloseTo(5, 6);
    const behindLeft = offscreenIndicator(identity, [-1, 0, 5]);
    expect(behindLeft.visible).toBe(false);
    expect(Math.abs(behindLeft.angleRad)).toBeCloseTo(Math.PI, 6);
    expect(offscreenIndicator(identity, [0, 5, -1]).angleRad).toBeCloseTo(Math.PI / 2, 6);
  });

  it('accounts for the phone orientation', () => {
    /* Yaw +90 deg about +Y: the camera now looks down world -X. */
    const s = Math.SQRT1_2;
    const yawed = { position: [0, 0, 0] as Vec3, orientation: [0, s, 0, s] as const };
    expect(offscreenIndicator(yawed, [-5, 0, 0]).visible).toBe(true);
    expect(offscreenIndicator(yawed, [0, 0, -5]).visible).toBe(false);
    expect(offscreenIndicator(yawed, [0, 0, -5]).angleRad).toBeCloseTo(0, 6);
  });
});
