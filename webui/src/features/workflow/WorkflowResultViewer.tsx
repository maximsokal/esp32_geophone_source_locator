import { useEffect, useMemo, useState } from 'react';
import {
  fetchDeviceStatus,
  fetchLocalizationRuns,
} from '../../api/deviceApi';
import type { LocalizationSessionPodGeometry } from '../../api/deviceApi';
import type { SavedSeismicSnapshot } from '../../domain/device';
import { fetchResultSegment, resultViewSourceDistances } from '../../domain/resultSegment';
import { useLocale, useT } from '../../i18n';
import { formatDistance, formatNumber } from '../../i18n/format';
import { materialNameKey } from '../objects/material';
import { envelopePolylinePoints } from '../seismic/savedWaveform';
import { WORKFLOW_UNKNOWN } from './workflowViewModel';

interface ResultData {
  snapshot: SavedSeismicSnapshot | null;
  pods: LocalizationSessionPodGeometry[];
  distances: (number | null)[];
  material: string | null;
}

interface Props {
  objectId: string | null;
  sessionId: string | null;
}

export default function WorkflowResultViewer({ objectId, sessionId }: Props) {
  const { locale } = useLocale();
  const t = useT();
  const [data, setData] = useState<ResultData | null>(null);
  const [loading, setLoading] = useState(true);
  const [failed, setFailed] = useState(false);

  useEffect(() => {
    let cancelled = false;
    setData(null);
    setFailed(false);
    setLoading(true);

    const load = async (): Promise<void> => {
      try {
        if (objectId === null || sessionId === null) {
          if (!cancelled) setData({ snapshot: null, pods: [], distances: [], material: null });
          return;
        }
        const [status, listing] = await Promise.all([
          fetchDeviceStatus(false, objectId),
          fetchLocalizationRuns(objectId, sessionId),
        ]);
        const summary = status.recording_sessions?.sessions.find((item) => item.session_id === sessionId);
        const pick = listing.picks
          .slice()
          .sort((a, b) => a.computed_at_ms - b.computed_at_ms)
          .at(-1);
        const run = listing.runs
          .filter((item) => item.solved && item.source_position !== undefined)
          .slice()
          .sort((a, b) => a.computed_at_ms - b.computed_at_ms)
          .at(-1);
        const distances = run?.source_position === undefined
          ? []
          : resultViewSourceDistances(run.source_position, listing.geometry.pods);
        const segment = summary === undefined || pick === undefined
          ? null
          : await fetchResultSegment(
            objectId,
            sessionId,
            pick.channels,
            summary.sample_rate_hz,
            summary.samples_written,
          );
        if (!cancelled) {
          setData({
            snapshot: segment?.session ?? null,
            pods: listing.geometry.pods,
            distances,
            material: run?.material ?? null,
          });
        }
      } catch {
        if (!cancelled) setFailed(true);
      } finally {
        if (!cancelled) setLoading(false);
      }
    };

    void load();
    return () => { cancelled = true; };
  }, [objectId, sessionId]);

  const lines = useMemo(() => data?.snapshot === null || data?.snapshot === undefined
    ? []
    : [0, 1, 2, 3].map((channel) => envelopePolylinePoints(
      data.snapshot!.points,
      channel,
      720,
      120,
      data.snapshot!.start_timestamp_us,
      data.snapshot!.end_timestamp_us,
    )), [data]);

  if (loading) return <p role="status">{t('workflow.result.loading')}</p>;
  if (failed) return <p role="alert">{t('workflow.result.load_failed')}</p>;
  if (data === null) return <p>{WORKFLOW_UNKNOWN}</p>;

  return (
    <div className="workflow-result-viewer">
      {data.material !== null && (
        <p>{t('workflow.material.run', { name: t(materialNameKey(data.material)) })}</p>
      )}
      <h3>{t('workflow.result.waveform')}</h3>
      {data.snapshot === null
        ? <p>{t('workflow.result.waveform_unavailable')}</p>
        : <div className="saved-seismic-stack">
          {[0, 1, 2, 3].map((channel) => (
            <div className="saved-seismic-row" key={channel}>
              <strong>{t('workflow.result.channel', { number: formatNumber(locale, channel + 1) })}</strong>
              <svg viewBox="0 0 720 120" role="img" aria-label={t('workflow.result.channel', { number: channel + 1 })} preserveAspectRatio="none">
                <line x1="0" y1="60" x2="720" y2="60" className="seismic-zero" />
                <path d={lines[channel]?.min ?? ''} className={`saved-seismic-line saved-min channel-${channel + 1}`} />
                <path d={lines[channel]?.max ?? ''} className={`saved-seismic-line saved-max channel-${channel + 1}`} />
              </svg>
            </div>
          ))}
        </div>}
      <h3>{t('workflow.result.sensor_distances')}</h3>
      {data.pods.length === 0
        ? <p>{WORKFLOW_UNKNOWN}</p>
        : <ul>
          {data.pods.map((pod, index) => {
            const distance = data.distances[index];
            return (
              <li key={`${pod.pod}-${index}`}>
                {t('workflow.result.remote_distance', { number: pod.pod })}:{' '}
                {distance === null || distance === undefined ? WORKFLOW_UNKNOWN : formatDistance(locale, distance)}
              </li>
            );
          })}
        </ul>}
    </div>
  );
}
