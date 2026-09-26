import type { DeviceStatus } from '../domain/device';

interface Props {
  status: DeviceStatus | null;
}

/**
 * MAX-179: announce only battery warnings or unavailable power telemetry.
 */
export function powerBannerText(status: DeviceStatus | null): string | null {
  const power = status?.power;
  if (status === null) return null;
  if (power === undefined) return 'Battery status unavailable.';
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
  switch (power.state) {
    case 'USB_ONLY_NO_BATTERY':
    case 'USB_CHARGING':
    case 'USB_BATTERY_READY':
    case 'BATTERY': return null;
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
