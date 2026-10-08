import type { DeviceStatus, ObjectStoreState } from '../../domain/device';
import { activeObject, connectedPodCount, formatCoordinate } from '../../domain/device';
import { useT, useLocale } from '../../i18n';
import { formatDistance, formatNumber } from '../../i18n/format';
import HardwareInventoryPanel from './HardwareInventoryPanel';

interface Props {
  status: DeviceStatus | null;
  objects: ObjectStoreState | null;
  error: string | null;
  onOpenObjects: () => void;
  onOpenSensors: () => void;
  onOpenActiveObject: () => void;
  onRefresh: () => void;
}

export default function DeviceDashboard({
  status,
  objects,
  error,
  onOpenObjects,
  onOpenSensors,
  onOpenActiveObject,
  onRefresh,
}: Props) {
  const t = useT();
  const { locale } = useLocale();
  const active = activeObject(objects);
  const connected = connectedPodCount(status);
  const systemState = status?.system.state ?? t('device.connecting');
  const acquisition = status?.acquisition;
  const batteryPercent = status?.power?.battery_percent_valid &&
    typeof status.power.battery_percent_estimate === 'number' &&
    Number.isFinite(status.power.battery_percent_estimate)
    ? `${formatNumber(locale, status.power.battery_percent_estimate, { maximumFractionDigits: 0 })}%`
    : t('power.state_unknown');
  const hardwareProblem = status !== null && status.system.state !== 'READY';
  const semanticStatus = status === null ? 'unavailable'
    : status.system.state === 'READY' ? 'normal'
      : status.system.state === 'FAULT' ? 'error' : 'warning';

  return (
    <section className="console-page dashboard-grid" data-state={semanticStatus} data-severity={semanticStatus}>
      {error !== null && <div className="console-alert" role="alert">{t('api.error_prefix', { error })}</div>}
      {hardwareProblem && (
        <div className="console-alert" role="status">
          {t('dashboard.hardware_alert', {
            state: status.system.state,
            acquisition: t(acquisition?.hardware_ready ? 'dashboard.acquisition_verified' : 'dashboard.acquisition_unverified'),
            ready: formatNumber(locale, status.system.ready_pods),
            required: formatNumber(locale, status.system.required_pods),
          })}{acquisition?.last_error_name && acquisition.last_error_name !== 'ESP_OK'
            ? `; ${t('dashboard.adc_error', { code: acquisition.last_error_name })}`
            : ''}.
        </div>
      )}

      <article className="console-card hero-card">
        <div className="eyebrow">{t('dashboard.eyebrow')}</div>
        <h1>{t('dashboard.title')}</h1>
        <p>{t('dashboard.description')}</p>
        <div className="console-actions">
          <button onClick={onOpenObjects}>{t('nav.objects')}</button>
          <button onClick={onOpenSensors}>{t('dashboard.sensor_pods')}</button>
          <button onClick={onRefresh} className="secondary">{t('common.refresh')}</button>
        </div>
      </article>

      <article className="console-card metric-card">
        <span>{t('dashboard.device')}</span><strong>{systemState}</strong>
        <small>{t('dashboard.mode', { mode: status?.system.mode ?? '—' })}</small>
      </article>
      <article className="console-card metric-card">
        <span>{t('dashboard.sensor_pods_metric')}</span><strong>{formatNumber(locale, connected)}/4</strong>
        <small>{status ? t('dashboard.ready_count', { count: formatNumber(locale, status.system.ready_pods) }) : t('dashboard.waiting_status')}</small>
      </article>
      <article className="console-card metric-card">
        <span>{t('dashboard.acquisition')}</span>
        <strong>{t(acquisition?.running ? 'dashboard.running' : acquisition?.hardware_ready ? 'dashboard.stopped' : 'dashboard.not_verified')}</strong>
        <small>{acquisition?.last_error_name && acquisition.last_error_name !== 'ESP_OK'
          ? acquisition.last_error_name
          : acquisition?.sample_rate_hz ? `${formatNumber(locale, acquisition.sample_rate_hz)} Hz · ${acquisition.source}` : t('dashboard.no_verified_source')}</small>
      </article>
      <article className="console-card metric-card">
        <span>{t('dashboard.battery_percent')}</span><strong>{batteryPercent}</strong>
      </article>
      <article className="console-card metric-card">
        <span>{t('dashboard.objects_metric')}</span><strong>{objects?.available ? formatNumber(locale, objects.objects.length) : '—'}</strong>
        <small>{t(objects?.available ? 'dashboard.stored_sd' : 'dashboard.store_unavailable')}</small>
      </article>

      <HardwareInventoryPanel status={status} />

      <article className="console-card wide-card">
        <div className="card-heading">
          <div><span className="eyebrow">{t('dashboard.active_object')}</span><h2>{active?.name ?? t('dashboard.no_active_object')}</h2></div>
          {active !== null && <button onClick={onOpenActiveObject}>{t('common.open')}</button>}
        </div>
        {active === null ? (
          <p>{t('dashboard.create_or_activate')}</p>
        ) : (
          <dl className="console-dl">
            <div><dt>{t('common.id')}</dt><dd>{active.id}</dd></div>
            <div><dt>{t('common.base_gps')}</dt><dd>{active.location.valid ? `${formatCoordinate(active.location.latitude)}, ${formatCoordinate(active.location.longitude)}` : t('common.not_bound')}</dd></div>
            <div><dt>{t('common.gps_accuracy')}</dt><dd>{active.location.valid ? `±${formatDistance(locale, active.location.accuracy_m, 1)}` : '—'}</dd></div>
            <div><dt>{t('common.cables')}</dt><dd>{active.cable_lengths_m.map((length) => formatDistance(locale, length, 1)).join(' · ')}</dd></div>
          </dl>
        )}
      </article>
    </section>
  );
}
