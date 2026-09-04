import { useEffect, useMemo, useState } from 'react';
import { fetchSeismic } from '../../api/deviceApi';
import type { SeismicSnapshot } from '../../domain/device';

const LIVE_REFRESH_MS = 500;
const LIVE_ERROR_BACKOFF_MIN_MS = 1500;
const LIVE_ERROR_BACKOFF_MAX_MS = 10000;

function polylinePoints(samples: number[], width: number, height: number): string {
  if (samples.length < 2) return '';
  let maxAbs = 1;
  for (const sample of samples) maxAbs = Math.max(maxAbs, Math.abs(sample));
  return samples.map((sample, index) => {
    const x = (index / (samples.length - 1)) * width;
    const y = height / 2 - (sample / maxAbs) * height * 0.44;
    return `${x.toFixed(1)},${y.toFixed(1)}`;
  }).join(' ');
}

export default function LiveSeismicPanel() {
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
    () => snapshot?.channels.map((channel) => polylinePoints(channel.samples_uv, 720, 120)) ?? [],
    [snapshot],
  );

  return (
    <section className="console-card seismic-live-card">
      <div className="card-heading">
        <div><span className="eyebrow">LIVE GEOPHONES</span><h3>{snapshot?.source ?? 'waiting for acquisition'}</h3></div>
        <small>{snapshot?.sample_rate_hz ? `${snapshot.sample_rate_hz} Hz` : '—'}</small>
      </div>
      {error !== null && <p className="console-error">{error}</p>}
      <div className="seismic-stack">
        {[0, 1, 2, 3].map((index) => (
          <div className="seismic-row" key={index}>
            <span>G{index + 1}</span>
            <svg viewBox="0 0 720 120" role="img" aria-label={`Geophone ${index + 1} live waveform`} preserveAspectRatio="none">
              <line x1="0" y1="60" x2="720" y2="60" className="seismic-zero" />
              <polyline points={lines[index] ?? ''} className={`seismic-line channel-${index + 1}`} />
            </svg>
          </div>
        ))}
      </div>
      <small>{snapshot ? `${snapshot.count} displayed · ${snapshot.total_frames.toLocaleString()} total frames` : 'No samples yet'}</small>
    </section>
  );
}
