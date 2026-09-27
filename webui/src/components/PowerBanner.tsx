import type { DeviceStatus } from '../domain/device';
import { translate, useLocale } from '../i18n';
import type { I18nLocale } from '../i18n';
import { formatNumber } from '../i18n/format';

interface Props {
  status: DeviceStatus | null;
}

/**
 * MAX-179: announce only battery warnings or unavailable power telemetry.
 */
export function powerBannerText(status: DeviceStatus | null, locale: I18nLocale = 'en'): string | null {
  const power = status?.power;
  if (status === null) return null;
  if (power === undefined) return translate(locale, 'power.status_unavailable');
  switch (power.low_battery) {
    case 'shutdown':
      return translate(locale, 'power.critical_shutdown');
    case 'grace':
      return power.shutdown_in_s === undefined
        ? translate(locale, 'power.critical_grace_unknown')
        : translate(locale, 'power.critical_grace', { seconds: formatNumber(locale, power.shutdown_in_s) });
    case 'warning':
      return translate(locale, 'web.power.low');
    default:
      break;
  }
  if (power.state === 'BATTERY_CRITICAL') return translate(locale, 'power.critical_now');
  if (power.state === 'BATTERY_LOW') return translate(locale, 'web.power.low');
  if (!power.valid || power.state === 'UNKNOWN' || power.state === undefined) {
    return translate(locale, 'power.status_unavailable');
  }
  switch (power.state) {
    case 'USB_ONLY_NO_BATTERY':
    case 'USB_CHARGING':
    case 'USB_BATTERY_READY':
    case 'BATTERY': return null;
    default: return translate(locale, 'power.status_unavailable');
  }
}

function powerBannerRole(status: DeviceStatus | null): 'alert' | 'status' {
  const power = status?.power;
  return power?.state === 'BATTERY_CRITICAL' || power?.state === 'BATTERY_LOW' ||
    power?.low_battery === 'warning' || power?.low_battery === 'grace' ||
    power?.low_battery === 'shutdown'
    ? 'alert'
    : 'status';
}

export default function PowerBanner({ status }: Props) {
  const { locale } = useLocale();
  const text = powerBannerText(status, locale);
  if (text === null) return null;
  return (
    <div className="console-alert power-banner" role={powerBannerRole(status)}>
      {text}
    </div>
  );
}
