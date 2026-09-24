import type { ObjectLocation } from './device';

/**
 * MAX-151: explicit frame bookkeeping (see docs/spatial-frames.md).
 *
 * Transforms are either measured (with source, time and uncertainty) or
 * recorded as unknown -- never assumed. Heading is never derived from a GPS
 * point.
 */

export type FrameId = 'gps' | 'sensor' | 'scan' | 'report';

export type TransformStatus = 'measured' | 'translation_only' | 'identity' | 'unknown';

export interface FrameTransform {
  from: FrameId;
  to: FrameId;
  status: TransformStatus;
  source: string;
  timestampMs: number | null;
  /** Translation uncertainty, metres (null = not applicable/unknown). */
  translationUncertaintyM: number | null;
  /** Rotation uncertainty, degrees (null = rotation unknown). */
  rotationUncertaintyDeg: number | null;
  note: string;
}

export interface FrameSummary {
  headingKnown: false;
  mirrorResolved: false;
  axisConvention: string;
  transforms: FrameTransform[];
}

export const SENSOR_AXIS_CONVENTION =
  'Sensor frame: MAIN at the origin, +X towards the first placed REMOTE, +Z up (relative barometric altitude), mirror side arbitrary, heading unknown.';

export function describeFrames(location: ObjectLocation | undefined, scanAvailable: boolean): FrameSummary {
  const gpsValid = location !== undefined && location.valid &&
    Number.isFinite(location.latitude) && Number.isFinite(location.longitude);
  return {
    headingKnown: false,
    mirrorResolved: false,
    axisConvention: SENSOR_AXIS_CONVENTION,
    transforms: [
      {
        from: 'sensor',
        to: 'gps',
        status: gpsValid ? 'translation_only' : 'unknown',
        source: gpsValid ? `phone GPS (${location?.source ?? 'unknown'})` : 'none',
        timestampMs: gpsValid ? location?.captured_at_ms ?? null : null,
        translationUncertaintyM: gpsValid && Number.isFinite(location?.accuracy_m) ? location?.accuracy_m ?? null : null,
        rotationUncertaintyDeg: null,
        note: gpsValid
          ? 'MAIN placed at the GPS fix; no heading -- a single GPS point cannot orient the layout.'
          : 'No stored GPS fix.',
      },
      {
        from: 'scan',
        to: 'sensor',
        status: 'unknown',
        source: 'none',
        timestampMs: null,
        translationUncertaintyM: null,
        rotationUncertaintyDeg: null,
        note: scanAvailable
          ? 'Scan saved but not registered: needs AprilTag/ArUco anchors (MAX-145).'
          : 'No scan saved for this recording.',
      },
      {
        from: 'report',
        to: 'sensor',
        status: 'identity',
        source: 'definition',
        timestampMs: null,
        translationUncertaintyM: 0,
        rotationUncertaintyDeg: 0,
        note: 'Localization results and report projections use the sensor frame of the recording.',
      },
    ],
  };
}
