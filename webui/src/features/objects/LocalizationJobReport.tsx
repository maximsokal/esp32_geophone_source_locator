import {
  jobProgressText,
  jobTimingRows,
  stackText,
  type LocalizationJobStatus,
} from '../../domain/localizationJob';
import { useLocale, useT } from '../../i18n';
import { formatNumber, formatPercent } from '../../i18n/format';

/**
 * Progress of the background "Calculate source" job while it runs, and the
 * device's own measurements once it is done: every method's wall time
 * (slowest first; the ones the time budget skipped are marked) and the
 * worker stack's high-water mark.
 */
export default function LocalizationJobReport({ status }: { status: LocalizationJobStatus }) {
  const { locale } = useLocale();
  const t = useT();
  const finished = status.state === 'succeeded' || status.state === 'failed';
  const rows = finished ? jobTimingRows(status) : [];
  const stack = finished ? stackText(status, locale) : '';
  return (
    <section className="localization-job" aria-label={t('calc.job_aria')} aria-live="polite">
      <p className="overlay-note" data-job-state={status.state}>{jobProgressText(status, locale)}</p>
      {rows.length > 0 && (
        <details className="settings-details">
          <summary>{t('calc.job_measured', { count: formatNumber(locale, rows.length) })}</summary>
          <table className="overlay-table">
            <thead><tr><th>{t('calc.job_method')}</th><th>{t('calc.job_time')}</th><th>{t('calc.job_share')}</th></tr></thead>
            <tbody>
              {rows.map((row) => (
                <tr key={row.method} className={row.skipped_for_time ? 'unavailable' : undefined}>
                  <td>{row.method}</td>
                  <td>{row.skipped_for_time ? t('calc.job_not_started') : `${formatNumber(locale, row.ms)} ms`}</td>
                  <td>{row.skipped_for_time ? '—' : formatPercent(locale, row.share)}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </details>
      )}
      {stack !== '' && (
        <p className={status.stack.margin_ok ? 'overlay-note' : 'console-alert'} role={status.stack.margin_ok ? undefined : 'alert'}>
          {stack}
        </p>
      )}
    </section>
  );
}
