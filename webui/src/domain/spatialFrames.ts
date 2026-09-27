import type { ObjectLocation } from './device';
import { translate, type I18nLocale } from '../i18n';

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

export function describeFrames(location: ObjectLocation | undefined, scanAvailable: boolean, locale: I18nLocale = 'en'): FrameSummary {
  const gpsValid = location !== undefined && location.valid &&
    Number.isFinite(location.latitude) && Number.isFinite(location.longitude);
  return {
    headingKnown: false,
    mirrorResolved: false,
    axisConvention: translate(locale, 'report.frame.sensor_axis'),
    transforms: [
      {
        from: 'sensor',
        to: 'gps',
        status: gpsValid ? 'translation_only' : 'unknown',
        source: gpsValid ? translate(locale, 'report.frame.phone_gps', { source: location?.source ?? 'unknown' }) : translate(locale, 'report.frame.no_source'),
        timestampMs: gpsValid ? location?.captured_at_ms ?? null : null,
        translationUncertaintyM: gpsValid && Number.isFinite(location?.accuracy_m) ? location?.accuracy_m ?? null : null,
        rotationUncertaintyDeg: null,
        note: gpsValid
          ? translate(locale, 'report.frame.sensor_to_gps_measured')
          : translate(locale, 'report.frame.sensor_to_gps_unknown'),
      },
      {
        from: 'scan',
        to: 'sensor',
        status: 'unknown',
        source: translate(locale, 'report.frame.no_source'),
        timestampMs: null,
        translationUncertaintyM: null,
        rotationUncertaintyDeg: null,
        note: scanAvailable
          ? translate(locale, 'report.frame.scan_to_sensor_registered')
          : translate(locale, 'report.frame.scan_to_sensor_missing'),
      },
      {
        from: 'report',
        to: 'sensor',
        status: 'identity',
        source: translate(locale, 'report.frame.definition'),
        timestampMs: null,
        translationUncertaintyM: 0,
        rotationUncertaintyDeg: 0,
        note: translate(locale, 'report.frame.report_to_sensor'),
      },
    ],
  };
}
