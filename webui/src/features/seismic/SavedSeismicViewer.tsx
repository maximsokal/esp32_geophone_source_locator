import { useEffect, useMemo, useState } from 'react';
import { fetchSavedSeismicSession } from '../../api/deviceApi';
import type {
  RecordingSessionSummary,
  SavedSeismicSnapshot,
} from '../../domain/device';
import { useLocale, useT } from '../../i18n';
import { formatNumber } from '../../i18n/format';
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

function formatUv(value: number, locale: 'en' | 'uk'): string {
  const absolute = Math.abs(value);
  if (absolute >= 1000) return `${formatNumber(locale, value / 1000, { minimumFractionDigits: 2, maximumFractionDigits: 2 })} mV`;
  return `${formatNumber(locale, value, { minimumFractionDigits: 1, maximumFractionDigits: 1 })} µV`;
}

function terminalLabel(snapshot: SavedSeismicSnapshot, t: (key: 'seismic.status.complete' | 'seismic.status.recovered_error' | 'seismic.status.error') => string): string {
  switch (snapshot.status) {
    case 'complete': return t('seismic.status.complete');
    case 'recovered_error': return t('seismic.status.recovered_error');
    case 'error': return t('seismic.status.error');
    default: return snapshot.status;
  }
}

export default function SavedSeismicViewer({ objectId, sessionId, summary, onClose }: Props) {
  const { locale } = useLocale();
  const t = useT();
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
          <span className="eyebrow">{t('seismic.saved_heading')}</span>
          <h2>{sessionId}</h2>
          <small>
            {objectId} · {formatNumber(locale, summary.sample_rate_hz)} Hz · {
              summary.recording_ms_available === false
                ? t('seismic.duration_unknown')
                : formatDuration(summary.recording_ms)
            }
          </small>
        </div>
        <button type="button" className="secondary" onClick={onClose}>{t('seismic.back_to_sessions')}</button>
      </div>

      <div className="console-actions saved-seismic-controls">
        <button type="button" className="secondary" onClick={reloadOverview} disabled={loading}>{t('seismic.overview')}</button>
        <button type="button" className="secondary" onClick={zoomIn} disabled={loading || !canNavigate || windowCount <= 1}>{t('seismic.zoom_in')}</button>
        <button type="button" className="secondary" onClick={zoomOut} disabled={loading || !canNavigate || windowCount >= totalFrames}>{t('seismic.zoom_out')}</button>
        <button type="button" className="secondary" onClick={() => move(-1)} disabled={loading || !canNavigate || windowStart === 0}>{t('seismic.previous_range')}</button>
        <button type="button" className="secondary" onClick={() => move(1)} disabled={loading || !canNavigate || windowStart + windowCount >= totalFrames}>{t('seismic.next_range')}</button>
      </div>

      {loading && <p className="console-loading saved-seismic-message">{t('seismic.loading')}</p>}
      {error !== null && (
        <div className="console-error saved-seismic-message" role="alert">
          <span>{error}</span>
          <button type="button" onClick={() => load(windowStart, windowCount)}>{t('seismic.retry')}</button>
        </div>
      )}

      {snapshot !== null && error === null && (
        <>
          {!snapshot.complete && (
            <div className="console-alert" role="status">
              {snapshot.recovered
                ? t('seismic.recovered_notice')
                : t('seismic.error_notice')}
            </div>
          )}
          <dl className="console-dl saved-seismic-meta">
            <div><dt>{t('seismic.source')}</dt><dd>{snapshot.source}</dd></div>
            <div><dt>{t('seismic.status')}</dt><dd>{terminalLabel(snapshot, t)}</dd></div>
            <div><dt>{t('seismic.readable')}</dt><dd>{t(snapshot.readable ? 'report.yes' : 'report.no')}</dd></div>
            <div><dt>{t('seismic.complete')}</dt><dd>{t(snapshot.complete ? 'report.yes' : 'report.no')}</dd></div>
            <div><dt>{t('seismic.frames')}</dt><dd>{formatNumber(locale, snapshot.start_frame)}–{formatNumber(locale, snapshot.start_frame + snapshot.frame_count)} / {formatNumber(locale, snapshot.source_frame_count)}</dd></div>
            <div><dt>{t('seismic.displayed_buckets')}</dt><dd>{formatNumber(locale, snapshot.count)}</dd></div>
            <div><dt>{t('seismic.duration')}</dt><dd>{snapshot.recording_ms_available ? formatDuration(snapshot.recording_ms) : t('seismic.unknown_after_recovery')}</dd></div>
            <div><dt>{t('seismic.dropped_frames')}</dt><dd>{snapshot.dropped_frames_available ? formatNumber(locale, snapshot.dropped_frames) : t('seismic.unknown_after_recovery')}</dd></div>
            <div><dt>{t('seismic.cadence')}</dt><dd>{snapshot.cadence.available ? `${formatNumber(locale, snapshot.cadence.collapsed_periods)} ${t('seismic.collapsed')}` : t('seismic.unknown_after_recovery')}</dd></div>
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
                    <strong>{t('seismic.channel', { number: formatNumber(locale, channel + 1) })}</strong>
                    <small>{formatUv(min, locale)} … {formatUv(max, locale)}</small>
                  </div>
                  <svg viewBox="0 0 720 120" role="img" aria-label={t('seismic.saved_waveform', { number: channel + 1 })} preserveAspectRatio="none">
                    <line x1="0" y1="60" x2="720" y2="60" className="seismic-zero" />
                    <path d={line?.min ?? ''} className={`saved-seismic-line saved-min channel-${channel + 1}`} />
                    <path d={line?.max ?? ''} className={`saved-seismic-line saved-max channel-${channel + 1}`} />
                  </svg>
                </div>
              );
            })}
          </div>
          <small className="saved-seismic-axis">
            {t('seismic.relative_range', {
              start: formatNumber(locale, (snapshot.start_timestamp_us - snapshot.start_timestamp_us) / 1e6, { minimumFractionDigits: 3, maximumFractionDigits: 3 }),
              end: formatNumber(locale, (snapshot.end_timestamp_us - snapshot.start_timestamp_us) / 1e6, { minimumFractionDigits: 3, maximumFractionDigits: 3 }),
            })}
          </small>
        </>
      )}
    </section>
  );
}

export { envelopePolylinePoints } from './savedWaveform';
