import { useEffect, useMemo, useState } from 'react';
import { fetchSavedSeismicSession } from '../../api/deviceApi';
import type {
  RecordingSessionSummary,
  SavedSeismicSnapshot,
} from '../../domain/device';
import { envelopePolylinePoints } from './savedWaveform';
import './SavedSeismicViewer.css';

interface Props {
  objectId: string;
  sessionId: string;
  summary: RecordingSessionSummary;
  onClose: () => void;
}

function formatDuration(milliseconds: number): string {
  const seconds = Math.max(0, Math.floor(milliseconds / 1000));
  return `${Math.floor(seconds / 60)}:${(seconds % 60).toString().padStart(2, '0')}`;
}

function formatUv(value: number): string {
  const absolute = Math.abs(value);
  if (absolute >= 1000) return `${(value / 1000).toFixed(2)} mV`;
  return `${value.toFixed(1)} uV`;
}

function terminalLabel(snapshot: SavedSeismicSnapshot): string {
  switch (snapshot.status) {
    case 'complete': return 'COMPLETE';
    case 'recovered_error': return 'RECOVERED ERROR';
    case 'error': return 'ERROR';
    default: return 'UNKNOWN';
  }
}

export default function SavedSeismicViewer({ objectId, sessionId, summary, onClose }: Props) {
  const [snapshot, setSnapshot] = useState<SavedSeismicSnapshot | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(true);
  const [range, setRange] = useState({ start: 0, count: 0 });

  const load = (start: number, count: number): void => {
    setLoading(true);
    setError(null);
    void fetchSavedSeismicSession(objectId, sessionId, {
      startFrame: start,
      ...(count > 0 ? { frameCount: count } : {}),
      points: 240,
    }).then((next) => {
      setSnapshot(next);
      setRange({ start: next.start_frame, count: next.frame_count });
    }).catch((reason: unknown) => {
      setError(reason instanceof Error ? reason.message : String(reason));
    }).finally(() => setLoading(false));
  };

  useEffect(() => {
    setSnapshot(null);
    setRange({ start: 0, count: 0 });
    load(0, 0);
    // The viewer is keyed by session identity from the parent.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [objectId, sessionId]);

  const totalFrames = snapshot?.source_frame_count ?? summary.samples_written;
  const windowCount = snapshot?.frame_count ?? range.count;
  const windowStart = snapshot?.start_frame ?? range.start;
  const canNavigate = totalFrames > 0 && windowCount > 0;

  const lines = useMemo(() => snapshot === null
    ? []
    : [0, 1, 2, 3].map((channel) => envelopePolylinePoints(
      snapshot.points,
      channel,
      720,
      120,
      snapshot.start_timestamp_us,
      snapshot.end_timestamp_us,
    )), [snapshot]);

  const reloadOverview = (): void => load(0, 0);
  const zoomIn = (): void => {
    if (!canNavigate || windowCount <= 1) return;
    const nextCount = Math.max(1, Math.floor(windowCount / 2));
    const center = windowStart + Math.floor(windowCount / 2);
    load(Math.max(0, center - Math.floor(nextCount / 2)), nextCount);
  };
  const zoomOut = (): void => {
    if (!canNavigate || windowCount >= totalFrames) return;
    const nextCount = Math.min(totalFrames, windowCount * 2);
    const center = windowStart + Math.floor(windowCount / 2);
    load(Math.max(0, Math.min(totalFrames - nextCount, center - Math.floor(nextCount / 2))), nextCount);
  };
  const move = (direction: -1 | 1): void => {
    if (!canNavigate || windowCount >= totalFrames) return;
    const nextStart = Math.max(0, Math.min(totalFrames - windowCount, windowStart + direction * windowCount));
    load(nextStart, windowCount);
  };

  return (
    <section className="console-card saved-seismic-viewer">
      <div className="card-heading">
        <div>
          <span className="eyebrow">SAVED SESSION</span>
          <h2>{sessionId}</h2>
          <small>
            {objectId} · {summary.sample_rate_hz.toLocaleString()} Hz · {
              summary.recording_ms_available === false
                ? 'duration unknown'
                : formatDuration(summary.recording_ms)
            }
          </small>
        </div>
        <button type="button" className="secondary" onClick={onClose}>Back to sessions</button>
      </div>

      <div className="console-actions saved-seismic-controls">
        <button type="button" className="secondary" onClick={reloadOverview} disabled={loading}>Overview</button>
        <button type="button" className="secondary" onClick={zoomIn} disabled={loading || !canNavigate || windowCount <= 1}>Zoom in</button>
        <button type="button" className="secondary" onClick={zoomOut} disabled={loading || !canNavigate || windowCount >= totalFrames}>Zoom out</button>
        <button type="button" className="secondary" onClick={() => move(-1)} disabled={loading || !canNavigate || windowStart === 0}>Previous range</button>
        <button type="button" className="secondary" onClick={() => move(1)} disabled={loading || !canNavigate || windowStart + windowCount >= totalFrames}>Next range</button>
      </div>

      {loading && <p className="console-loading saved-seismic-message">Loading saved waveform…</p>}
      {error !== null && (
        <div className="console-error saved-seismic-message" role="alert">
          <span>{error}</span>
          <button type="button" onClick={() => load(windowStart, windowCount)}>Retry</button>
        </div>
      )}

      {snapshot !== null && error === null && (
        <>
          {!snapshot.complete && (
            <div className="console-alert" role="status">
              {snapshot.recovered
                ? 'Recovered persisted frames after an interrupted/failed recording. Data is readable, but this session was not completed successfully.'
                : 'This recording ended with an error. Only the persisted frame prefix reported by terminal metadata is shown.'}
            </div>
          )}
          <dl className="console-dl saved-seismic-meta">
            <div><dt>Source</dt><dd>{snapshot.source}</dd></div>
            <div><dt>Status</dt><dd>{terminalLabel(snapshot)}</dd></div>
            <div><dt>Readable</dt><dd>{snapshot.readable ? 'yes' : 'no'}</dd></div>
            <div><dt>Complete</dt><dd>{snapshot.complete ? 'yes' : 'no'}</dd></div>
            <div><dt>Frames</dt><dd>{snapshot.start_frame.toLocaleString()}–{(snapshot.start_frame + snapshot.frame_count).toLocaleString()} / {snapshot.source_frame_count.toLocaleString()}</dd></div>
            <div><dt>Displayed buckets</dt><dd>{snapshot.count.toLocaleString()}</dd></div>
            <div><dt>Duration</dt><dd>{snapshot.recording_ms_available ? formatDuration(snapshot.recording_ms) : 'unknown after recovery'}</dd></div>
            <div><dt>Dropped frames</dt><dd>{snapshot.dropped_frames_available ? snapshot.dropped_frames.toLocaleString() : 'unknown after recovery'}</dd></div>
            <div><dt>Cadence</dt><dd>{snapshot.cadence.available ? `${snapshot.cadence.collapsed_periods.toLocaleString()} collapsed` : 'unknown'}</dd></div>
          </dl>
          <div className="saved-seismic-stack">
            {[0, 1, 2, 3].map((channel) => {
              const line = lines[channel];
              const envelope = snapshot.points.map((point) => point.channels[channel]);
              const min = envelope.length > 0
                ? envelope.reduce((value, point) => Math.min(value, point.min_uv), Infinity)
                : 0;
              const max = envelope.length > 0
                ? envelope.reduce((value, point) => Math.max(value, point.max_uv), -Infinity)
                : 0;
              return (
                <div className="saved-seismic-row" key={channel}>
                  <div className="saved-seismic-label">
                    <strong>CH{channel + 1}</strong>
                    <small>{formatUv(min)} … {formatUv(max)}</small>
                  </div>
                  <svg viewBox="0 0 720 120" role="img" aria-label={`Saved geophone ${channel + 1} waveform`} preserveAspectRatio="none">
                    <line x1="0" y1="60" x2="720" y2="60" className="seismic-zero" />
                    <path d={line?.min ?? ''} className={`saved-seismic-line saved-min channel-${channel + 1}`} />
                    <path d={line?.max ?? ''} className={`saved-seismic-line saved-max channel-${channel + 1}`} />
                  </svg>
                </div>
              );
            })}
          </div>
          <small className="saved-seismic-axis">
            Relative range: {((snapshot.start_timestamp_us - snapshot.start_timestamp_us) / 1e6).toFixed(3)}s–{((snapshot.end_timestamp_us - snapshot.start_timestamp_us) / 1e6).toFixed(3)}s · envelope min/max; units shown per channel
          </small>
        </>
      )}
    </section>
  );
}

export { envelopePolylinePoints } from './savedWaveform';
