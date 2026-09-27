import { useEffect, useMemo, useState } from 'react';
import { fetchSeismic } from '../../api/deviceApi';
import type { SeismicSnapshot } from '../../domain/device';
import { useLocale, useT } from '../../i18n';
import { formatNumber } from '../../i18n/format';

const LIVE_REFRESH_MS = 500;
const LIVE_ERROR_BACKOFF_MIN_MS = 1500;
const LIVE_ERROR_BACKOFF_MAX_MS = 10000;

function normalizedWaveformPoints(
  samples: number[],
  timestampsMs: number[],
  width: number,
  height: number,
): string[] | null {
  if (samples.length === 0 || samples.length !== timestampsMs.length) return [];
  if (
    !Number.isFinite(width) || width < 0 ||
    !Number.isFinite(height) || height < 0 ||
    !samples.every(Number.isFinite) ||
    !timestampsMs.every(Number.isFinite)
  ) return null;

  for (let index = 1; index < timestampsMs.length; index += 1) {
    if (timestampsMs[index] < timestampsMs[index - 1]) return null;
  }

  let maxAbs = 1;
  for (const sample of samples) maxAbs = Math.max(maxAbs, Math.abs(sample));
  const firstTimestamp = timestampsMs[0];
  const lastTimestamp = timestampsMs[timestampsMs.length - 1];
  const span = lastTimestamp - firstTimestamp;
  const toX = (timestamp: number): number => span > 0
    ? ((timestamp - firstTimestamp) / span) * width
    : width / 2;

  return samples.map((sample, index) => {
    const x = toX(timestampsMs[index]);
    const y = height / 2 - (sample / maxAbs) * height * 0.44;
    return `${x.toFixed(1)},${y.toFixed(1)}`;
  });
}

/**
 * Timestamp-aware point geometry kept as a small pure helper for tests and
 * callers that need the complete coordinate list. The live renderer below
 * uses polylineSegments() so it never draws through a proven missed period.
 */
export function polylinePoints(
  samples: number[],
  timestampsMs: number[],
  width: number,
  height: number,
): string {
  const points = normalizedWaveformPoints(samples, timestampsMs, width, height);
  return points === null ? '' : points.join(' ');
}

/**
 * Split a live trace only when the timestamp distance spans at least two
 * configured sample periods. This is not an arbitrary jitter threshold: two
 * periods means at least one configured sample instant has no returned frame.
 * Smaller timing variation remains connected and is still positioned by its
 * real timestamp.
 */
export function polylineSegments(
  samples: number[],
  timestampsMs: number[],
  width: number,
  height: number,
  sampleRateHz: number,
): string[] {
  if (!Number.isFinite(sampleRateHz) || sampleRateHz <= 0) return [];

  const points = normalizedWaveformPoints(samples, timestampsMs, width, height);
  if (points === null || points.length === 0) return [];

  const missedPeriodBoundaryMs = (2 * 1000) / sampleRateHz;
  const segments: string[][] = [[]];

  for (let index = 0; index < points.length; index += 1) {
    if (
      index > 0 &&
      timestampsMs[index] - timestampsMs[index - 1] >= missedPeriodBoundaryMs
    ) {
      segments.push([]);
    }
    segments[segments.length - 1].push(points[index]);
  }

  return segments
    .filter((segment) => segment.length > 0)
    .map((segment) => segment.join(' '));
}

export default function LiveSeismicPanel() {
  const { locale } = useLocale();
  const t = useT();
  const [snapshot, setSnapshot] = useState<SeismicSnapshot | null>(null);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    let cancelled = false;
    let timer: number | null = null;
    let consecutiveFailures = 0;

    const schedule = (delay: number): void => {
      if (cancelled) return;
      timer = window.setTimeout(() => void refresh(), delay);
    };

    const refresh = async (): Promise<void> => {
      if (cancelled) return;
      if (document.visibilityState !== 'visible') {
        schedule(2000);
        return;
      }

      try {
        const next = await fetchSeismic(220);
        if (cancelled) return;
        consecutiveFailures = 0;
        setSnapshot(next);
        setError(null);
        schedule(LIVE_REFRESH_MS);
      }
      catch (reason) {
        if (cancelled) return;
        consecutiveFailures = Math.min(consecutiveFailures + 1, 5);
        setError(reason instanceof Error ? reason.message : String(reason));
        const delay = Math.min(
          LIVE_ERROR_BACKOFF_MAX_MS,
          LIVE_ERROR_BACKOFF_MIN_MS * (2 ** (consecutiveFailures - 1)),
        );
        schedule(delay);
      }
    };

    void refresh();
    return () => {
      cancelled = true;
      if (timer !== null) window.clearTimeout(timer);
    };
  }, []);

  const lines = useMemo(
    () => snapshot?.channels.map((channel) => polylineSegments(
      channel.samples_uv,
      snapshot.timestamps_ms,
      720,
      120,
      snapshot.sample_rate_hz,
    )) ?? [],
    [snapshot],
  );

  return (
    <section className="console-card seismic-live-card">
      <div className="card-heading">
        <div><span className="eyebrow">{t('seismic.live_heading')}</span><h3>{snapshot?.source ?? t('seismic.waiting_acquisition')}</h3></div>
        <small>{snapshot?.sample_rate_hz ? `${formatNumber(locale, snapshot.sample_rate_hz)} Hz` : '—'}</small>
      </div>
      {error !== null && <p className="console-error">{error}</p>}
      <div className="seismic-stack">
        {[0, 1, 2, 3].map((index) => (
          <div className="seismic-row" key={index}>
            <span>G{index + 1}</span>
            <svg viewBox="0 0 720 120" role="img" aria-label={t('seismic.live_waveform', { number: index + 1 })} preserveAspectRatio="none">
              <line x1="0" y1="60" x2="720" y2="60" className="seismic-zero" />
              {(lines[index] ?? []).map((points, segmentIndex) => (
                <polyline
                  key={segmentIndex}
                  points={points}
                  className={`seismic-line channel-${index + 1}`}
                />
              ))}
            </svg>
          </div>
        ))}
      </div>
      <small>{snapshot ? t('seismic.live_frames', { displayed: formatNumber(locale, snapshot.count), total: formatNumber(locale, snapshot.total_frames) }) : t('seismic.no_samples')}</small>
    </section>
  );
}
