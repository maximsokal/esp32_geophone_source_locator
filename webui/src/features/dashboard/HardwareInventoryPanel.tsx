import type { DeviceHardwareStatus, DeviceStatus } from '../../domain/device';
import { useT, type I18nKey } from '../../i18n';

interface Props {
  status: DeviceStatus | null;
}

const MODULE_KEYS: Readonly<Record<string, string>> = {
  sd_card: 'web.hw.module.sd_card',
  ad7606: 'web.hw.module.ad7606',
  sensor_mux: 'web.hw.module.sensor_mux',
  main_xiao: 'web.hw.module.main_xiao',
  barometer: 'web.hw.module.barometer',
  power_monitor: 'web.hw.module.power_monitor',
};

const STATE_KEYS: Readonly<Record<string, string>> = {
  unknown: 'web.hw.state.unknown',
  present: 'web.hw.state.present',
  absent: 'web.hw.state.absent',
  fault: 'web.hw.state.fault',
  degraded: 'web.hw.state.degraded',
};

function translatedOrRaw(value: string, keys: Readonly<Record<string, string>>, t: (key: I18nKey) => string): string {
  const key = Object.prototype.hasOwnProperty.call(keys, value) ? keys[value] : undefined;
  return key === undefined ? value : t(key as I18nKey);
}

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
          const knownState = Object.prototype.hasOwnProperty.call(STATE_KEYS, module.state);
          const stateClass = knownState ? module.state : 'unknown';
          return (
            <li className="hardware-module-item" key={`${module.id}-${index}`}>
              <div className="hardware-module-heading">
                <strong>{translatedOrRaw(module.id, MODULE_KEYS, t)}</strong>
                <span className={`hardware-state-badge hardware-state-${stateClass}`}>
                  {translatedOrRaw(module.state, STATE_KEYS, t)}
                </span>
              </div>
              <small>{t((module.required ? 'web.hw.required' : 'web.hw.optional') as I18nKey)}</small>
              {typeof module.reason === 'string' && module.reason.length > 0 && (
                <p className="hardware-module-reason">{module.reason}</p>
              )}
            </li>
          );
        })}
      </ul>
    </article>
  );
}
