import type {
  DeviceHardwareStatus,
  DeviceStatus,
  HardwareModuleId,
  HardwareModuleState,
  HardwareReasonCode,
} from '../../domain/device';
import { useT, type I18nKey } from '../../i18n';
import { normalizeHardwareModule } from './hardwareInventoryModel';

interface Props {
  status: DeviceStatus | null;
}

const MODULE_KEYS: Readonly<Record<HardwareModuleId, I18nKey>> = {
  sd_card: 'web.hw.module.sd_card',
  ad7606: 'web.hw.module.ad7606',
  sensor_mux: 'web.hw.module.sensor_mux',
  main_xiao: 'web.hw.module.main_xiao',
  barometer: 'web.hw.module.barometer',
  power_monitor: 'web.hw.module.power_monitor',
};

const STATE_KEYS: Readonly<Record<HardwareModuleState, I18nKey>> = {
  unknown: 'web.hw.state.unknown',
  present: 'web.hw.state.present',
  absent: 'web.hw.state.absent',
  fault: 'web.hw.state.fault',
  degraded: 'web.hw.state.degraded',
};

const REASON_KEYS: Readonly<Record<HardwareReasonCode, I18nKey>> = {
  ready: 'web.hw.reason.ready' as I18nKey,
  no_response: 'web.hw.reason.no_response' as I18nKey,
  no_filesystem: 'web.hw.reason.no_filesystem' as I18nKey,
  mount_failed: 'web.hw.reason.mount_failed' as I18nKey,
  read_probe_failed: 'web.hw.reason.read_probe_failed' as I18nKey,
  topology_probe_ok: 'web.hw.reason.topology_probe_ok' as I18nKey,
  topology_probe_fault: 'web.hw.reason.topology_probe_fault' as I18nKey,
  no_upstream_response: 'web.hw.reason.no_upstream_response' as I18nKey,
  sensor_invalid: 'web.hw.reason.sensor_invalid' as I18nKey,
  first_sample_ok: 'web.hw.reason.first_sample_ok' as I18nKey,
  no_valid_sample: 'web.hw.reason.no_valid_sample' as I18nKey,
  busy_responds: 'web.hw.reason.busy_responds' as I18nKey,
  no_busy_response: 'web.hw.reason.no_busy_response' as I18nKey,
  presence_probe_failed: 'web.hw.reason.presence_probe_failed' as I18nKey,
  blocked_5v_rail_low: 'web.hw.reason.blocked_5v_rail_low' as I18nKey,
  init_failed: 'web.hw.reason.init_failed' as I18nKey,
  start_failed: 'web.hw.reason.start_failed' as I18nKey,
  sensor_ok: 'web.hw.reason.sensor_ok' as I18nKey,
  upstream_target_assigned: 'web.hw.reason.upstream_target_assigned' as I18nKey,
  upstream_topology_fault: 'web.hw.reason.upstream_topology_fault' as I18nKey,
  none: 'web.hw.reason.none' as I18nKey,
  unrecognized: 'web.hw.reason.unrecognized' as I18nKey,
};

export default function HardwareInventoryPanel({ status }: Props) {
  const t = useT();
  const hardware: DeviceHardwareStatus | undefined = status?.hardware;
  const modules = Array.isArray(hardware?.modules) ? hardware.modules : [];

  if (hardware === undefined) return null;

  return (
    <article className="console-card wide-card hardware-inventory-card">
      <div className="card-heading">
        <h2>{t('web.hw.title' as I18nKey)}</h2>
      </div>
      {hardware.required_missing === true && (
        <div className="console-alert hardware-required-banner" role="alert">
          {t('web.hw.degraded_banner' as I18nKey)}
        </div>
      )}
      <ul className="hardware-module-list">
        {modules.map((module, index) => {
          const normalized = normalizeHardwareModule(module);
          const stateKey = normalized.state === 'unrecognized'
            ? 'web.hw.state.unrecognized' as I18nKey
            : STATE_KEYS[normalized.state];
          return (
            <li className="hardware-module-item" key={`${normalized.id}-${index}`}>
              <div className="hardware-module-heading">
                <strong>{normalized.knownId ? t(MODULE_KEYS[normalized.id]) : normalized.id}</strong>
                <span className={`hardware-state-badge hardware-state-${normalized.state}`}>
                  {t(stateKey)}
                </span>
              </div>
              <small>{t((module.required ? 'web.hw.required' : 'web.hw.optional') as I18nKey)}</small>
              <p className="hardware-module-reason">{t(REASON_KEYS[normalized.reasonCode])}</p>
              {normalized.reasonCode === 'unrecognized' && normalized.rawReason.length > 0 && (
                <small className="hardware-module-reason hardware-module-reason-diagnostics">
                  {normalized.rawReason}
                </small>
              )}
            </li>
          );
        })}
      </ul>
    </article>
  );
}
