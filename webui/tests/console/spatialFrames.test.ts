import { describe, expect, it } from 'vitest';

import { describeFrames } from '../../src/domain/spatialFrames';

/* MAX-151: frames are explicit; heading is never taken from GPS. */

const gps = {
  valid: true, latitude: 50.45, longitude: 30.52, accuracy_m: 6,
  altitude_valid: false, altitude_m: 0, altitude_accuracy_m: 0, captured_at_ms: 1234, source: 'phone',
};

describe('spatial frames', () => {
  it('uses GPS for translation only, never for heading', () => {
    const frames = describeFrames(gps, true);
    expect(frames.headingKnown).toBe(false);
    expect(frames.mirrorResolved).toBe(false);
    const toGps = frames.transforms.find((t) => t.from === 'sensor' && t.to === 'gps')!;
    expect(toGps.status).toBe('translation_only');
    expect(toGps.rotationUncertaintyDeg).toBeNull();
    expect(toGps.translationUncertaintyM).toBe(6);
    expect(toGps.timestampMs).toBe(1234);
    expect(toGps.note).toContain('no heading');
  });

  it('records the scan<->sensor transform as unknown until registration exists', () => {
    const scan = describeFrames(gps, true).transforms.find((t) => t.from === 'scan')!;
    expect(scan.status).toBe('unknown');
    expect(scan.note).toContain('MAX-145');
    expect(describeFrames(gps, false).transforms.find((t) => t.from === 'scan')!.note).toContain('No scan');
  });

  it('marks everything unknown without a GPS fix and keeps the report frame = sensor frame', () => {
    const frames = describeFrames({ ...gps, valid: false }, false);
    expect(frames.transforms.find((t) => t.to === 'gps')!.status).toBe('unknown');
    expect(frames.transforms.find((t) => t.from === 'report')!.status).toBe('identity');
    expect(frames.axisConvention).toContain('mirror side arbitrary');
    expect(describeFrames(undefined, false).transforms[0].status).toBe('unknown');
  });
});
