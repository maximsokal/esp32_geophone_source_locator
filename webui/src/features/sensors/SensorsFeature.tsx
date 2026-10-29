import { lazy, Suspense } from 'react';
import type { DeviceStatus, ObjectProfile } from '../../domain/device';
import LevelingCard from './LevelingCard';
import '../nrflog/NrfLogPanel.css';
import './sensors.css';
import { useLocale, useT } from '../../i18n';
import { formatNumber } from '../../i18n/format';

const NrfLogPanel = lazy(() => import('../nrflog/NrfLogPanel'));

interface Props {
  status: DeviceStatus | null;
  object: ObjectProfile | null;
  error: string | null;
  onCalibrateHeights: () => Promise<boolean>;
}

function measured(valid: boolean, text: string, unavailable: string): string {
  return valid ? text : unavailable;
}

const POD_STATE_KEYS: Record<string, 'sensors.pod_state.disconnected' | 'sensors.pod_state.ready' | 'sensors.pod_state.warning' | 'sensors.pod_state.error'> = {
  DISCONNECTED: 'sensors.pod_state.disconnected',
  READY: 'sensors.pod_state.ready',
  WARNING: 'sensors.pod_state.warning',
  ERROR: 'sensors.pod_state.error',
};

const NODE_LINK_STATUS_KEYS: Record<string, 'sensors.node_link.not_connected' | 'sensors.node_link.connected' | 'sensors.node_link.ready' | 'sensors.node_link.measuring' | 'sensors.node_link.stale'> = {
  STALE: 'sensors.node_link.stale',
  NOT_CONNECTED: 'sensors.node_link.not_connected',
  CONNECTED: 'sensors.node_link.connected',
  READY: 'sensors.node_link.ready',
  MEASURING: 'sensors.node_link.measuring',
};

const NODE_LINK_STATUS_CLASSES: Record<string, string> = {
  STALE: 'connected',
  NOT_CONNECTED: 'not-connected',
  CONNECTED: 'connected',
  READY: 'ready',
  MEASURING: 'measuring',
};

export default function SensorsFeature({ status, object, error, onCalibrateHeights }: Props) {
  const t = useT();
  const { locale } = useLocale();
  const pods = status?.pods ?? [];
  const stalePowerChannels = (status?.power.channels ?? []).filter((channel) => channel.stale === true);
  const nodeLinks = status?.node_links ?? [];
  const acquisition = status?.acquisition;
  const serial = acquisition?.serial;
  const ad7606State = status?.hardware?.modules?.find((module) => module.id === 'ad7606')?.state;
  const lastErrorName = acquisition?.last_error_name;
  const adcError = lastErrorName && lastErrorName !== 'ESP_OK'
    ? lastErrorName
    : acquisition?.last_error !== undefined && acquisition.last_error !== 0
      ? String(acquisition.last_error)
      : undefined;
  const hasExtendedAdcStatus = status?.hardware !== undefined
    || acquisition?.last_error !== undefined
    || acquisition?.last_error_name !== undefined;
  const adcStatus = ad7606State === 'absent'
    ? t('sensors.not_detected')
    : adcError !== undefined
      ? t('sensors.adc_failed', { name: adcError })
      : acquisition?.hardware_ready
        ? t('sensors.detected')
        : !hasExtendedAdcStatus
          ? t('sensors.not_detected')
          : acquisition?.running !== true
            ? t('sensors.adc_not_started')
            : t('dashboard.acquisition_unverified');
  const streamStuck = serial?.stream_stuck === true || serial?.stream_state === 'STUCK';
  const serialInterference = serial?.suspicious === true || serial?.stream_state === 'SUSPICIOUS';
  const powerFault = serial?.power_fault_latched === true;
  const overcurrentFault = powerFault && serial?.power_gate === 'overcurrent';
  const powerInterlock = serial?.power_gate === undefined
    ? t('sensors.not_reported')
    : overcurrentFault
      ? t('sensors.power_overcurrent', { current: formatNumber(locale, serial.power_fault_current_a ?? 0, { minimumFractionDigits: 3, maximumFractionDigits: 3 }) })
      : powerFault
      ? t('sensors.power_fault', { voltage: formatNumber(locale, serial.power_fault_rail_v ?? 0, { minimumFractionDigits: 2, maximumFractionDigits: 2 }) })
      : serial.power_gate === 'ok'
        ? t('sensors.power_verified')
        : serial.power_gate === 'unverified'
          ? t('sensors.power_unverified_reading')
          : serial.power_gate;
  const overcurrentChannels = (status?.power.channels ?? []).filter(
    (channel) => channel.overcurrent === 'warning' || channel.overcurrent === 'critical',
  );
  const serialStream = serial === undefined
    ? t('sensors.not_observed')
    : streamStuck
      ? t('sensors.adc_data_stuck', { pattern: serial.pattern })
      : serialInterference
        ? t('sensors.serial_interference', { pattern: serial.pattern })
        : serial.pattern;

  return (
    <section className="console-page sensors-page">
      {error !== null && <div className="console-alert" role="alert">{t('api.error_prefix', { error })}</div>}
      {powerFault && !overcurrentFault && (
        <div className="console-alert" role="alert">
          {t('sensors.power_fault_alert')}
        </div>
      )}
      {overcurrentFault && (
        <div className="console-alert" role="alert">
          {t('sensors.overcurrent_alert')}
        </div>
      )}
      {overcurrentChannels.map((channel) => (
        <div className="console-alert" role="alert" key={`oc-${channel.id}`}>
          {t(channel.overcurrent === 'critical' ? 'sensors.channel_overcurrent_critical' : 'sensors.channel_overcurrent_warning', {
            channel: channel.name,
            current: formatNumber(locale, channel.current_a, { minimumFractionDigits: 3, maximumFractionDigits: 3 }),
            latched: channel.overcurrent_latched ? t('sensors.latched_suffix') : '',
          })}.
        </div>
      ))}
      {stalePowerChannels.map((channel) => (
        <div className="console-alert warning" role="status" key={`stale-${channel.id}`}>
          {t('sensors.power_snapshot_stale', { channel: channel.name, age: formatNumber(locale, channel.age_ms ?? 0) })}
        </div>
      ))}

      <div className="section-heading">
        <div><span className="eyebrow">{t('sensors.hardware_diagnostics')}</span><h1>{t('sensors.acquisition_four_remotes')}</h1></div>
        <p>{t('sensors.ready_evidence')}</p>
      </div>

      <article className="console-card wide-card">
        <div className="card-heading">
          <div><span className="eyebrow">{t('sensors.base_adc')}</span><h2>{acquisition?.source ?? t('sensors.acquisition_source')}</h2></div>
          <span className={`state-pill ${acquisition?.hardware_ready ? 'ready' : 'offline'}`}>
            {adcStatus}
          </span>
        </div>
        <dl className="console-dl">
          <div><dt>{t('sensors.hardware_proof')}</dt><dd>{t(acquisition?.hardware_ready ? 'sensors.busy_verified' : 'sensors.no_verified_pulse')}</dd></div>
          <div><dt>{t('sensors.acquisition')}</dt><dd>{t(acquisition?.running ? 'sensors.running' : acquisition?.requested_running ? 'sensors.requested_unhealthy' : 'sensors.stopped')}</dd></div>
          <div><dt>{t('sensors.last_error')}</dt><dd>{acquisition?.last_error_name && acquisition.last_error_name !== 'ESP_OK' ? acquisition.last_error_name : t('sensors.none')}</dd></div>
          <div><dt>{t('sensors.sample_rate')}</dt><dd>{acquisition?.sample_rate_hz ? `${formatNumber(locale, acquisition.sample_rate_hz)} Hz` : '—'}</dd></div>
          <div><dt>{t('sensors.power_interlock')}</dt><dd>{powerInterlock}</dd></div>
          <div><dt>{t('sensors.serial_stream')}</dt><dd>{serialStream}</dd></div>
          <div><dt>{t('sensors.stream_state')}</dt><dd>{serial?.stream_state ?? t('sensors.not_observed')}</dd></div>
          <div><dt>{t('sensors.serial_quality')}</dt><dd>{serial?.quality ?? t('sensors.not_observed')}</dd></div>
          <div><dt>{t('sensors.signal_quality')}</dt><dd>{acquisition?.signal?.quality ?? t('sensors.not_observed')}</dd></div>
          <div><dt>{t('sensors.serial_streak')}</dt><dd>{serial ? t('sensors.abnormal_busy', { count: formatNumber(locale, serial.suspicious_streak), level: serial.busy_level }) : t('sensors.not_observed')}</dd></div>
        </dl>
      </article>

      <LevelingCard gate={status?.recording_gate} main={status?.main} pods={pods} onCalibrateHeights={onCalibrateHeights} />

      {(nodeLinks.length > 0 || status?.topology?.stale === true) && (
        <article className="console-card wide-card node-link-card">
          <div className="card-heading">
            <div><span className="eyebrow">{t('sensors.node_link.heading')}</span><h2>{t('sensors.node_link.mapping')}</h2></div>
          </div>
          {status?.topology?.stale === true && (
            <p className="console-alert warning" role="status">
              {t('sensors.topology_snapshot_stale', { age: formatNumber(locale, status.topology.age_ms) })}
            </p>
          )}
          <div className="node-link-grid">
            {nodeLinks.map((link) => {
              const statusKey = NODE_LINK_STATUS_KEYS[link.status];
              const statusClass = NODE_LINK_STATUS_CLASSES[link.status] ?? 'unknown';
              return (
                <div className="node-link-row" key={`${link.channel}-${link.node}`}>
                  <strong>{t('sensors.node_link.channel', { channel: link.channel })} → {link.node}</strong>
                  <span className={`node-link-status node-link-status--${statusClass}`}>
                    {statusKey === undefined
                      ? t('sensors.node_link.unknown', { status: link.status })
                      : t(statusKey)}
                  </span>
                </div>
              );
            })}
          </div>
        </article>
      )}

      <Suspense fallback={null}><NrfLogPanel status={status} /></Suspense>

      <div className="pod-grid">
        {[0, 1, 2, 3].map((index) => {
          const pod = pods[index];
          const connected = pod !== undefined && pod.state !== 'DISCONNECTED';
          const stale = pod?.node_link_status === 'STALE' && pod.state !== 'ERROR';
          const podState = pod?.state ?? 'DISCONNECTED';
          const podStateKey = POD_STATE_KEYS[podState];
          return (
            <article className="console-card pod-card" key={index}>
              <div className="card-heading">
                <h2>{t('sensors.pod', { number: formatNumber(locale, index + 1) })}</h2>
                <span className={`state-pill ${stale ? 'warning' : connected ? 'ready' : 'offline'}`}>
                  {stale ? t('sensors.node_link.stale') : podStateKey === undefined
                    ? t('sensors.pod_state.unknown', { state: podState })
                    : t(podStateKey)}
                </span>
              </div>
              <dl className="console-dl compact">
                <div><dt>{t('sensors.cable')}</dt><dd>{formatNumber(locale, object?.cable_lengths_m[index] ?? 5, { minimumFractionDigits: 2, maximumFractionDigits: 2 })} m</dd></div>
                <div><dt>{t('sensors.pressure')}</dt><dd>{pod ? measured(pod.barometer.valid, `${formatNumber(locale, pod.barometer.pressure_pa / 100, { minimumFractionDigits: 1, maximumFractionDigits: 1 })} hPa`, t('common.not_available')) : t('common.not_available')}</dd></div>
                <div><dt>{t('sensors.relative_height')}</dt><dd>{pod ? measured(pod.barometer.valid, `${formatNumber(locale, pod.barometer.relative_altitude_cm / 100, { minimumFractionDigits: 2, maximumFractionDigits: 2 })} m`, t('common.not_available')) : t('common.not_available')}</dd></div>
                <div><dt>{t('sensors.temperature')}</dt><dd>{pod ? measured(pod.environment.temperature_valid, `${formatNumber(locale, pod.environment.temperature_c, { minimumFractionDigits: 1, maximumFractionDigits: 1 })} °C`, t('common.not_available')) : t('common.not_available')}</dd></div>
                <div><dt>{t('sensors.humidity')}</dt><dd>{pod ? measured(pod.environment.humidity_valid, `${formatNumber(locale, pod.environment.humidity_percent, { minimumFractionDigits: 1, maximumFractionDigits: 1 })} %`, t('common.not_available')) : t('common.not_available')}</dd></div>
                <div><dt>{t('sensors.roll_pitch')}</dt><dd>{pod ? measured(pod.imu.valid, `${formatNumber(locale, pod.imu.roll_deg, { minimumFractionDigits: 1, maximumFractionDigits: 1 })}° / ${formatNumber(locale, pod.imu.pitch_deg, { minimumFractionDigits: 1, maximumFractionDigits: 1 })}°`, t('common.not_available')) : t('common.not_available')}</dd></div>
                <div><dt>{t('sensors.yaw')}</dt><dd>{pod ? measured(pod.imu.valid, `${formatNumber(locale, pod.imu.yaw_relative_deg, { minimumFractionDigits: 1, maximumFractionDigits: 1 })}° ${t('sensors.relative')}`, t('common.not_available')) : t('common.not_available')}</dd></div>
                <div><dt>{t('sensors.geophone_instant')}</dt><dd>{pod ? measured(pod.geophone.valid, `${formatNumber(locale, pod.geophone.instant_uv, { minimumFractionDigits: 2, maximumFractionDigits: 2 })} µV`, t('common.not_available')) : t('common.not_available')}</dd></div>
                <div><dt>{t('sensors.geophone_rms')}</dt><dd>{pod ? measured(pod.geophone.valid, `${formatNumber(locale, pod.geophone.rms_uv, { minimumFractionDigits: 2, maximumFractionDigits: 2 })} µV`, t('common.not_available')) : t('common.not_available')}</dd></div>
                <div><dt>{t('sensors.local_xy')}</dt><dd>{pod ? measured(pod.position.local_valid, `${formatNumber(locale, pod.position.local_x_m, { minimumFractionDigits: 2, maximumFractionDigits: 2 })} / ${formatNumber(locale, pod.position.local_y_m, { minimumFractionDigits: 2, maximumFractionDigits: 2 })} m`, t('common.not_available')) : t('common.not_available')}</dd></div>
                <div><dt>{t('sensors.global_coordinates')}</dt><dd>{pod?.position.global_valid
                  ? `${formatNumber(locale, pod.position.latitude, { minimumFractionDigits: 6, maximumFractionDigits: 6 })}, ${formatNumber(locale, pod.position.longitude, { minimumFractionDigits: 6, maximumFractionDigits: 6 })}${pod.position.altitude_valid ? ` / ${formatNumber(locale, pod.position.altitude_m, { minimumFractionDigits: 1, maximumFractionDigits: 1 })} m` : ''}`
                  : object?.location.valid
                    ? t('sensors.base_gps_pod_unavailable')
                    : t('common.not_available')}</dd></div>
              </dl>
            </article>
          );
        })}
      </div>
    </section>
  );
}
