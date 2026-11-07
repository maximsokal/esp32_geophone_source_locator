import { useEffect, useMemo, useState } from 'react';
import {
  ApiRequestOwnerRetiredError,
  ApiRequestPreemptedError,
  createRequestOwner,
  fetchSeismic,
  retireRequestOwner,
} from '../../api/deviceApi';
import type { DeviceNodeLink, SeismicSnapshot } from '../../domain/device';
import { useLocale, useT } from '../../i18n';
import { formatNumber } from '../../i18n/format';
import { envelopePolylinePoints } from './savedWaveform';
import { NODE_LINK_STATUS_KEYS, nodeLinkClass, waveformLinkStatus } from '../sensors/nodeLinkStatus';

const LIVE_REFRESH_MS = 500;
const LIVE_ERROR_BACKOFF_MIN_MS = 1500;
const LIVE_ERROR_BACKOFF_MAX_MS = 10000;

interface Props {
  /** Current link statuses (status.node_links); presentation only, never changes the waveform data. */
  nodeLinks?: readonly DeviceNodeLink[];
  /** Last status fetch failed: the link colours are the last known. */
  statusStale?: boolean;
}

export default function LiveSeismicPanel({ nodeLinks, statusStale = false }: Props) {
  const { locale } = useLocale();
  const t = useT();
  const [snapshot, setSnapshot] = useState<SeismicSnapshot | null>(null);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    let cancelled = false;
    let timer: number | null = null;
    let consecutiveFailures = 0;
    const owner = createRequestOwner();

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
        const next = await fetchSeismic({ spanMs: 440, maxPoints: 220, owner });
        if (cancelled) return;
        consecutiveFailures = 0;
        setSnapshot(next);
        setError(null);
        schedule(LIVE_REFRESH_MS);
      }
      catch (reason) {
        if (cancelled || reason instanceof ApiRequestOwnerRetiredError) return;
        if (reason instanceof ApiRequestPreemptedError) {
          schedule(LIVE_REFRESH_MS);
          return;
        }
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
      retireRequestOwner(owner);
    };
  }, []);

  const lines = useMemo(() => {
    if (snapshot === null || snapshot.points.length === 0) return [];
    const rangeStartUs = snapshot.points[0].timestamp_us;
    const rangeEndUs = snapshot.points[snapshot.points.length - 1].end_timestamp_us;
    return [0, 1, 2, 3].map((channel) => envelopePolylinePoints(
      snapshot.points,
      channel,
      720,
      120,
      rangeStartUs,
      rangeEndUs,
    ));
  }, [snapshot]);

  return (
    <section className="console-card seismic-live-card">
      <div className="card-heading">
        <div><span className="eyebrow">{t('seismic.live_heading')}</span><h3>{snapshot?.source ?? t('seismic.waiting_acquisition')}</h3></div>
        <small>{snapshot?.sample_rate_hz ? `${formatNumber(locale, snapshot.sample_rate_hz)} Hz` : '—'}</small>
      </div>
      {error !== null && <p className="console-error">{error}</p>}
      <div className="seismic-stack">
        {[0, 1, 2, 3].map((index) => {
          const linkStatus = waveformLinkStatus(nodeLinks, index);
          const statusKey = linkStatus === null ? undefined : NODE_LINK_STATUS_KEYS[linkStatus];
          const linkLabel = linkStatus === null ? undefined
            : statusKey === undefined ? t('sensors.node_link.unknown', { status: linkStatus }) : t(statusKey);
          return (
          <div className="seismic-row" key={index}>
            <span
              className={`seismic-link seismic-link--${linkStatus === null ? 'unknown' : nodeLinkClass(linkStatus, statusStale)}`}
              title={linkLabel}
            >G{index + 1}{linkLabel === undefined ? null : <span className="sr-only"> {linkLabel}</span>}</span>
            <svg viewBox="0 0 720 120" role="img" aria-label={t('seismic.live_waveform', { number: index + 1 })} preserveAspectRatio="none">
              <line x1="0" y1="60" x2="720" y2="60" className="seismic-zero" />
              <path d={lines[index]?.min ?? ''} className={`saved-seismic-line saved-min channel-${index + 1}`} />
              <path d={lines[index]?.max ?? ''} className={`saved-seismic-line saved-max channel-${index + 1}`} />
            </svg>
          </div>
          );
        })}
      </div>
      <small>{snapshot ? t('seismic.live_frames', { displayed: formatNumber(locale, snapshot.count), total: formatNumber(locale, snapshot.total_frames) }) : t('seismic.no_samples')}</small>
    </section>
  );
}
