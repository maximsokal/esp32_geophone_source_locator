import type { BleNodeEstimate, Vec3 } from '../xr/bleRoughPlacement';
import { offscreenIndicator } from '../xr/bleRoughPlacement';
import { useLocale, useT } from '../i18n';
import { formatNumber, formatPercent } from '../i18n/format';

/*
 * MAX-146: scanner HUD for pre-anchor node hints. RSSI-only states are shown
 * as "~" distances / rough clouds with their uncertainty; nothing here is a
 * sensor position (Channel Sounding and visual anchors stay authoritative).
 */

interface Props {
  estimates: readonly BleNodeEstimate[];
  pose: { position: Vec3; orientation: readonly [number, number, number, number] } | null;
}

function stateText(e: BleNodeEstimate, t: ReturnType<typeof useT>, locale: 'en' | 'uk'): string {
  switch (e.state) {
    case 'not_detected': return t('ble.not_heard');
    case 'ble_detected':
      return e.ringM === null ? t('ble.heard') : t('ble.walk_to_localize', { minimum: formatNumber(locale, e.ringM.minM, { maximumFractionDigits: 0 }), maximum: formatNumber(locale, e.ringM.maxM, { maximumFractionDigits: 0 }) });
    case 'rough': return t('ble.rough', { uncertainty: formatNumber(locale, e.uncertaintyM ?? 0, { minimumFractionDigits: 1, maximumFractionDigits: 1 }) });
    case 'anchored': return t('ble.visual_anchor');
  }
}

export function BleRoughPlacementHud({ estimates, pose }: Props) {
  const { locale } = useLocale();
  const t = useT();
  return (
    <div className="xr-ble-hud" aria-label={t('ble.aria_label')}>
      <div className="xr-ble-chips">
        {estimates.map((e) => (
          <span key={e.nodeId} className={`xr-ble-chip ${e.state}`} title={t('ble.sample_title', { count: formatNumber(locale, e.sampleCount), confidence: formatPercent(locale, e.confidence) })}>
            <strong>{e.label}</strong> {stateText(e, t, locale)}
          </span>
        ))}
        <span className="xr-ble-note">{t('ble.note')}</span>
      </div>
      {pose !== null && estimates.filter((e) => (e.state === 'rough' || e.state === 'anchored') && e.centre !== null).map((e) => {
        const indicator = offscreenIndicator(pose, e.centre!);
        if (indicator.visible) return null;
        /* Screen y grows downwards. */
        const x = 50 + 42 * Math.cos(indicator.angleRad);
        const y = 50 - 42 * Math.sin(indicator.angleRad);
        return (
          <div key={`arrow-${e.nodeId}`} className={`xr-ble-arrow ${e.state}`} style={{ left: `${x}%`, top: `${y}%` }}
            aria-label={t('ble.offscreen', { label: e.label, distance: formatNumber(locale, indicator.distanceM, { maximumFractionDigits: 0 }) })}>
            <span style={{ transform: `rotate(${-indicator.angleRad}rad)` }} aria-hidden="true">➜</span>
            {e.label} ~{formatNumber(locale, indicator.distanceM, { maximumFractionDigits: 0 })} m
          </div>
        );
      })}
    </div>
  );
}
