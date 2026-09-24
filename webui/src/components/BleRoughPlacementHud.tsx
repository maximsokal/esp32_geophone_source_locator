import type { BleNodeEstimate, Vec3 } from '../xr/bleRoughPlacement';
import { offscreenIndicator } from '../xr/bleRoughPlacement';

/*
 * MAX-146: scanner HUD for pre-anchor node hints. RSSI-only states are shown
 * as "~" distances / rough clouds with their uncertainty; nothing here is a
 * sensor position (Channel Sounding and visual anchors stay authoritative).
 */

interface Props {
  estimates: readonly BleNodeEstimate[];
  pose: { position: Vec3; orientation: readonly [number, number, number, number] } | null;
}

function stateText(e: BleNodeEstimate): string {
  switch (e.state) {
    case 'not_detected': return 'not heard';
    case 'ble_detected':
      return e.ringM === null ? 'BLE heard' : `~${e.ringM.minM.toFixed(0)}–${e.ringM.maxM.toFixed(0)} m · walk to localize`;
    case 'rough': return `rough ±${(e.uncertaintyM ?? 0).toFixed(1)} m`;
    case 'anchored': return 'visual anchor ✓';
  }
}

export function BleRoughPlacementHud({ estimates, pose }: Props) {
  return (
    <div className="xr-ble-hud" aria-label="BLE node hints">
      <div className="xr-ble-chips">
        {estimates.map((e) => (
          <span key={e.nodeId} className={`xr-ble-chip ${e.state}`} title={`${e.sampleCount} RSSI samples · confidence ${Math.round(e.confidence * 100)}%`}>
            <strong>{e.label}</strong> {stateText(e)}
          </span>
        ))}
        <span className="xr-ble-note">RSSI hints are rough (±metres) and never replace Channel Sounding.</span>
      </div>
      {pose !== null && estimates.filter((e) => (e.state === 'rough' || e.state === 'anchored') && e.centre !== null).map((e) => {
        const indicator = offscreenIndicator(pose, e.centre!);
        if (indicator.visible) return null;
        /* Screen y grows downwards. */
        const x = 50 + 42 * Math.cos(indicator.angleRad);
        const y = 50 - 42 * Math.sin(indicator.angleRad);
        return (
          <div key={`arrow-${e.nodeId}`} className={`xr-ble-arrow ${e.state}`} style={{ left: `${x}%`, top: `${y}%` }}
            aria-label={`${e.label} off screen, about ${indicator.distanceM.toFixed(0)} m`}>
            <span style={{ transform: `rotate(${-indicator.angleRad}rad)` }} aria-hidden="true">➜</span>
            {e.label} ~{indicator.distanceM.toFixed(0)} m
          </div>
        );
      })}
    </div>
  );
}
