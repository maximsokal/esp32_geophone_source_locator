import { translate, type I18nLocale } from '../i18n';
import { formatDistance, formatNumber, formatPercent } from '../i18n/format';
import type { RefinementUnavailableReason, ScanDistanceRefinement } from '../xr/scanDistanceRefinement';

const UNAVAILABLE_KEYS = {
  no_registration: 'scan.refine.unavailable.no_registration',
  too_few_devices: 'scan.refine.unavailable.too_few_devices',
  no_geometry: 'scan.refine.unavailable.no_geometry',
  no_common_pairs: 'scan.refine.unavailable.no_common_pairs',
  geometry_from_tags: 'scan.refine.unavailable.geometry_from_tags',
} as const satisfies Record<RefinementUnavailableReason, Parameters<typeof translate>[1]>;

/** Millimetres below one metre, metres above: an error of 12 mm must not read as 0.01 m. */
function lengthText(locale: I18nLocale, mm: number): string {
  return Math.abs(mm) < 1000 ? `${formatNumber(locale, Math.round(mm))} mm` : formatDistance(locale, mm / 1000, 2);
}

function signedLengthText(locale: I18nLocale, mm: number): string {
  return `${mm > 0 ? '+' : ''}${lengthText(locale, mm)}`;
}

/**
 * The scan information screen: which devices the scan saw, which pair distances were recalculated from it and by how much the
 * error fell. The error model is printed under the numbers; see scanDistanceRefinement.ts.
 */
export function ScanDistanceRefinementCard({ refinement, locale }: { refinement: ScanDistanceRefinement; locale: I18nLocale }) {
  const t = (key: Parameters<typeof translate>[1], params?: Record<string, string>): string => translate(locale, key, params);
  const devices = refinement.scannedNodes.length === 0 ? '--' : refinement.scannedNodes.join(', ');
  return (
    <section className="panel-card scan-refinement" aria-label={t('scan.refine.title')} data-refinement-status={refinement.status}>
      <div className="panel-title">{t('scan.refine.title')}</div>
      {refinement.status === 'unavailable' ? (
        <p role="status">{t(UNAVAILABLE_KEYS[refinement.reason], { nodes: devices })}</p>
      ) : (
        <>
          <dl className="status-list">
            <div><dt>{t('scan.refine.devices')}</dt><dd>{devices}</dd></div>
            <div><dt>{t('scan.refine.pairs')}</dt><dd>{formatNumber(locale, refinement.summary.pairCount)}</dd></div>
          </dl>
          <ul className="scan-refinement-pairs">
            {refinement.pairs.map((pair) => (
              <li key={`${pair.a}-${pair.b}`} data-pair={`${pair.a}-${pair.b}`}>
                {t('scan.refine.pair', {
                  a: pair.a,
                  b: pair.b,
                  cs: lengthText(locale, pair.csMm),
                  scan: lengthText(locale, pair.scanMm),
                  correction: signedLengthText(locale, pair.correctionMm),
                  before: lengthText(locale, pair.beforeMm),
                  after: lengthText(locale, pair.afterMm),
                })}
              </li>
            ))}
          </ul>
          <p role="status" className="scan-refinement-summary" data-reduction-percent={refinement.summary.reductionPercent === null ? 'none' : refinement.summary.reductionPercent.toFixed(1)}>
            {refinement.summary.reductionPercent === null || refinement.summary.reductionPercent === 0
              ? t('scan.refine.no_reduction')
              : t('scan.refine.reduction', {
                percent: formatPercent(locale, refinement.summary.reductionPercent / 100, 1),
                before: lengthText(locale, refinement.summary.rmsBeforeMm),
                after: lengthText(locale, refinement.summary.rmsAfterMm),
              })}
          </p>
          <p className="scan-refinement-offset" data-offset-reliable={refinement.summary.offsetReliable ? 'yes' : 'no'}>
            {refinement.summary.offsetReliable
              ? t('scan.refine.offset', {
                offset: signedLengthText(locale, refinement.summary.offsetMm),
                spread: lengthText(locale, refinement.summary.spreadAfterOffsetMm),
                pairs: formatNumber(locale, refinement.summary.pairCount),
              })
              : t('scan.refine.offset_few', { pairs: formatNumber(locale, refinement.summary.pairCount) })}
          </p>
          {refinement.skippedNodes.length > 0 && <p>{t('scan.refine.skipped', { nodes: refinement.skippedNodes.join(', ') })}</p>}
          <p className="scan-refinement-model">{t('scan.refine.model')}</p>
        </>
      )}
    </section>
  );
}
