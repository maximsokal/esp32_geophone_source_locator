import type { ObjectLocation, ObjectProfile, RecordingSessionSummary } from './device';
import type { LocalizationRunListing } from '../api/deviceApi';
import { buildLocalizationOverlay } from './localizationOverlay';
import type { LocalizationOverlayModel } from './localizationOverlay';
import { buildRecordingEvidence } from './recordingEvidence';
import type { RecordingEvidence } from './recordingEvidence';
import { encodeQr, qrByteCapacity } from './qrCode';

/**
 * MAX-150: report for one saved recording of an object (printable page ->
 * browser "Save as PDF"). Built only from that recording's persisted
 * evidence; the object-level part lists recordings side by side without
 * merging their geometry or scans.
 */

export interface MapLinks {
  osm: string;
  google: string;
}

export function mapLinks(location: ObjectLocation | undefined): MapLinks | null {
  if (location === undefined || !location.valid ||
      !Number.isFinite(location.latitude) || !Number.isFinite(location.longitude) ||
      Math.abs(location.latitude) > 90 || Math.abs(location.longitude) > 180) {
    return null;
  }
  const lat = location.latitude.toFixed(6);
  const lon = location.longitude.toFixed(6);
  return {
    osm: `https://www.openstreetmap.org/?mlat=${lat}&mlon=${lon}#map=18/${lat}/${lon}`,
    google: `https://www.google.com/maps?q=${lat},${lon}`,
  };
}

/* ---- Projections ---- */

export type ProjectionAxis = 'x' | 'y' | 'z';

export interface ProjectedPoint { u: number; v: number }

export interface Projection {
  width: number;
  height: number;
  /** Map a 3D point (metres) into SVG coordinates. */
  map: (x: number, y: number, z: number) => ProjectedPoint;
  /** SVG units per metre (same on both axes). */
  scale: number;
}

function component(axis: ProjectionAxis, x: number, y: number, z: number): number {
  return axis === 'x' ? x : axis === 'y' ? y : z;
}

/**
 * Equal-aspect projection onto the (horizontal, vertical) axes; up is up
 * on paper. Bounds come from the given points plus optional radii.
 */
export function makeProjection(
  points: Array<{ x: number; y: number; z: number; r?: number }>,
  horizontal: ProjectionAxis,
  vertical: ProjectionAxis,
  width: number,
  height: number,
  padding = 16,
): Projection {
  let minU = Infinity;
  let maxU = -Infinity;
  let minV = Infinity;
  let maxV = -Infinity;
  for (const p of points) {
    const r = p.r ?? 0;
    const u = component(horizontal, p.x, p.y, p.z);
    const v = component(vertical, p.x, p.y, p.z);
    minU = Math.min(minU, u - r); maxU = Math.max(maxU, u + r);
    minV = Math.min(minV, v - r); maxV = Math.max(maxV, v + r);
  }
  if (!Number.isFinite(minU)) { minU = -1; maxU = 1; minV = -1; maxV = 1; }
  const spanU = Math.max(maxU - minU, 0.5);
  const spanV = Math.max(maxV - minV, 0.5);
  const scale = Math.min((width - 2 * padding) / spanU, (height - 2 * padding) / spanV);
  const centreU = (minU + maxU) / 2;
  const centreV = (minV + maxV) / 2;
  return {
    width,
    height,
    scale,
    map: (x, y, z) => ({
      u: width / 2 + (component(horizontal, x, y, z) - centreU) * scale,
      v: height / 2 - (component(vertical, x, y, z) - centreV) * scale,
    }),
  };
}

/** Evenly thin a point cloud for the printed projection. */
export function thinPoints(positions: Float32Array, maxPoints: number): Array<{ x: number; y: number; z: number }> {
  const count = Math.floor(positions.length / 3);
  const step = Math.max(1, Math.ceil(count / maxPoints));
  const result: Array<{ x: number; y: number; z: number }> = [];
  for (let i = 0; i < count; i += step) {
    const x = positions[i * 3];
    const y = positions[i * 3 + 1];
    const z = positions[i * 3 + 2];
    if (Number.isFinite(x) && Number.isFinite(y) && Number.isFinite(z)) result.push({ x, y, z });
  }
  return result;
}

/* ---- Report payload QR ---- */

export interface ReportQr {
  kind: 'full_payload' | 'reference';
  text: string;
}

/**
 * The whole result summary as a QR when it fits (version 10 / level M,
 * 213 bytes); otherwise a durable reference to the recording, with the
 * limitation stated.
 */
export function reportQr(
  object: ObjectProfile,
  sessionId: string,
  overlay: LocalizationOverlayModel,
): ReportQr {
  const selected = overlay.sectors.find((s) => s.emphasis === 'selected');
  const payload = JSON.stringify({
    o: object.id,
    s: sessionId,
    m: selected?.method ?? null,
    p: selected === undefined ? null : [
      Number(selected.position.x_m.toFixed(2)),
      Number(selected.position.y_m.toFixed(2)),
      Number(selected.position.z_m.toFixed(2)),
    ],
    r: selected === undefined ? null : Number(selected.radiusM.toFixed(2)),
    n: overlay.sectors.length,
  });
  if (new TextEncoder().encode(payload).length <= qrByteCapacity(10) && encodeQr(payload) !== null) {
    return { kind: 'full_payload', text: payload };
  }
  const reference = `geolocator:object=${object.id};session=${sessionId}`;
  return { kind: 'reference', text: reference };
}

/* ---- Report model ---- */

export interface RecordingReport {
  object: ObjectProfile;
  session: RecordingSessionSummary;
  generatedAtMs: number;
  links: MapLinks | null;
  evidence: RecordingEvidence;
  overlay: LocalizationOverlayModel;
  qr: ReportQr;
  /** Scan points in the SCAN frame (unregistered to sensors, MAX-145). */
  scanPoints: Array<{ x: number; y: number; z: number }>;
}

export function buildRecordingReport(
  object: ObjectProfile,
  session: RecordingSessionSummary,
  listing: LocalizationRunListing | null,
  scanPositions: Float32Array | null,
  selectedMethod: string | null,
  generatedAtMs: number,
): RecordingReport {
  const overlay = buildLocalizationOverlay(listing?.runs ?? [], listing?.geometry ?? null, selectedMethod);
  return {
    object,
    session,
    generatedAtMs,
    links: mapLinks(object.location),
    evidence: buildRecordingEvidence(
      listing,
      scanPositions === null ? { present: false, bytes: 0 } : { present: true, bytes: scanPositions.byteLength },
      null,
    ),
    overlay,
    qr: reportQr(object, session.session_id, overlay),
    scanPoints: scanPositions === null ? [] : thinPoints(scanPositions, 4000),
  };
}
