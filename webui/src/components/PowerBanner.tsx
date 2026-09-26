import type { DeviceStatus } from '../domain/device';

interface Props {
  status: DeviceStatus | null;
}

/**
 * MAX-179: render the ESP32's stable INA3221 state on every console page.
 */
export function powerBannerText(status: DeviceStatus | null): string | null {
  const power = status?.power;
  if (power === undefined) return null;
  switch (power.low_battery) {
    case 'shutdown':
      return 'Battery critical: the device is shutting down safely (recording finalised, SD flushed).';
    case 'grace':
      return `Battery critical: connect USB power within ${power.shutdown_in_s ?? 0} s or the device shuts down.`;
    case 'warning':
      return 'Battery low: connect USB power soon.';
    default:
      break;
  }
  if (power.state === 'BATTERY_CRITICAL') return 'Battery critical: connect USB power now.';
  if (power.state === 'BATTERY_LOW') return 'Battery low: connect USB power soon.';
  if (!power.valid || power.state === 'UNKNOWN' || power.state === undefined) {
    return 'Battery status unavailable.';
  }

  const percent = power.battery_percent_valid === true &&
    typeof power.battery_percent_estimate === 'number' &&
    Number.isFinite(power.battery_percent_estimate)
    ? ` (${Math.round(power.battery_percent_estimate)}%)`
    : '';
  switch (power.state) {
    case 'USB_ONLY_NO_BATTERY': return 'USB power connected; no battery detected.';
    case 'USB_CHARGING': return `Battery charging${percent}.`;
    case 'USB_BATTERY_READY':
      return power.battery_full
        ? 'Battery fully charged.'
        : `USB power connected; battery idle${percent}.`;
    case 'BATTERY': return `Running on battery${percent}.`;
    default: return 'Battery status unavailable.';
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
  const text = powerBannerText(status);
  if (text === null) return null;
  return (
    <div className="console-alert power-banner" role={powerBannerRole(status)}>
      {text}
    </div>
  );
}
