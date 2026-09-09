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
): SavedEnvelopePolyline {
  if (points.length === 0 || channelIndex < 0 || channelIndex >= 4) {
    return { min: '', max: '', scale_uv: 1 };
  }

  let maxAbs = 1;
  for (const point of points) {
    const channel = point.channels[channelIndex];
    maxAbs = Math.max(maxAbs, Math.abs(channel.min_uv), Math.abs(channel.max_uv));
  }
  const scale_uv = maxAbs * 1.15;
  const toY = (value: number): number => height / 2 - (value / scale_uv) * height * 0.44;
  const toX = (index: number): number => points.length === 1
    ? width / 2
    : (index / (points.length - 1)) * width;

  const min = points.map((point, index) => {
    const value = point.channels[channelIndex].min_uv;
    return `${toX(index).toFixed(1)},${toY(value).toFixed(1)}`;
  }).join(' ');
  const max = points.map((point, index) => {
    const value = point.channels[channelIndex].max_uv;
    return `${toX(index).toFixed(1)},${toY(value).toFixed(1)}`;
  }).join(' ');

  return { min, max, scale_uv };
}
