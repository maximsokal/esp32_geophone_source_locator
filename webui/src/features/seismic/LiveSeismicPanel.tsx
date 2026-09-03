import { useEffect, useMemo, useState } from 'react';
import { fetchSeismic } from '../../api/deviceApi';
import type { SeismicSnapshot } from '../../domain/device';

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
    const refresh = async () => {
      try {
        const next = await fetchSeismic(220);
        if (!cancelled) {
          setSnapshot(next);
          setError(null);
        }
      }
      catch (reason) {
        if (!cancelled) setError(reason instanceof Error ? reason.message : String(reason));
      }
    };
    void refresh();
    const timer = window.setInterval(() => void refresh(), 500);
    return () => {
      cancelled = true;
      window.clearInterval(timer);
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
