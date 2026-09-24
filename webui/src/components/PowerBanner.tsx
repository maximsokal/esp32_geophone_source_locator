import type { DeviceStatus } from '../domain/device';

interface Props {
  status: DeviceStatus | null;
}

/**
 * MAX-180/183: low-battery banner on every console page. The ESP32 decides
 * the state (INA3221 + hysteresis); this only renders it. Nothing is shown
 * while the battery is fine or the firmware does not report the state yet.
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
  return null;
}

export default function PowerBanner({ status }: Props) {
  const text = powerBannerText(status);
  if (text === null) return null;
  return (
    <div className="console-alert power-banner" role="alert">
      {text}
    </div>
  );
}
