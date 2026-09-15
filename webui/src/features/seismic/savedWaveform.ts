import type { SavedSeismicEnvelopePoint } from '../../domain/device';

export interface SavedEnvelopePolyline {
  min: string;
  max: string;
  scale_uv: number;
}

export function envelopePolylinePoints(
  points: SavedSeismicEnvelopePoint[],
  channelIndex: number,
  width: number,
  height: number,
  rangeStartUs: number,
  rangeEndUs: number,
): SavedEnvelopePolyline {
  if (
    points.length === 0 ||
    channelIndex < 0 || channelIndex >= 4 ||
    !Number.isFinite(width) || width < 0 ||
    !Number.isFinite(height) || height < 0 ||
    !Number.isFinite(rangeStartUs) ||
    !Number.isFinite(rangeEndUs) ||
    rangeEndUs < rangeStartUs
  ) {
    return { min: '', max: '', scale_uv: 1 };
  }

  let maxAbs = 1;
  let previousStart = -Infinity;
  let previousEnd = -Infinity;
  for (const point of points) {
    const channel = point.channels[channelIndex];
    if (
      channel === undefined ||
      !Number.isFinite(point.timestamp_us) ||
      !Number.isFinite(point.end_timestamp_us) ||
      point.end_timestamp_us < point.timestamp_us ||
      point.timestamp_us < previousStart ||
      point.end_timestamp_us < previousEnd ||
      !Number.isFinite(channel.min_uv) ||
      !Number.isFinite(channel.max_uv)
    ) {
      return { min: '', max: '', scale_uv: 1 };
    }
    previousStart = point.timestamp_us;
    previousEnd = point.end_timestamp_us;
    maxAbs = Math.max(maxAbs, Math.abs(channel.min_uv), Math.abs(channel.max_uv));
  }

  const scale_uv = maxAbs * 1.15;
  const toY = (value: number): number => height / 2 - (value / scale_uv) * height * 0.44;
  const spanUs = rangeEndUs - rangeStartUs;
  const toX = (timestampUs: number): number => spanUs > 0
    ? Math.min(width, Math.max(0, ((timestampUs - rangeStartUs) / spanUs) * width))
    : width / 2;

  const segmentPath = (kind: 'min_uv' | 'max_uv'): string => points.map((point) => {
    const value = point.channels[channelIndex][kind];
    return `M ${toX(point.timestamp_us).toFixed(1)} ${toY(value).toFixed(1)} L ${toX(point.end_timestamp_us).toFixed(1)} ${toY(value).toFixed(1)}`;
  }).join(' ');

  return {
    min: segmentPath('min_uv'),
    max: segmentPath('max_uv'),
    scale_uv,
  };
}
