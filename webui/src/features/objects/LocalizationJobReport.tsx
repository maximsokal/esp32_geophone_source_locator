import {
  jobProgressText,
  jobTimingRows,
  stackText,
  type LocalizationJobStatus,
} from '../../domain/localizationJob';

/**
 * Progress of the background "Calculate source" job while it runs, and the
 * device's own measurements once it is done: every method's wall time
 * (slowest first; the ones the time budget skipped are marked) and the
 * worker stack's high-water mark.
 */
export default function LocalizationJobReport({ status }: { status: LocalizationJobStatus }) {
  const finished = status.state === 'succeeded' || status.state === 'failed';
  const rows = finished ? jobTimingRows(status) : [];
  const stack = finished ? stackText(status) : '';
  return (
    <section className="localization-job" aria-label="Calculation on the device" aria-live="polite">
      <p className="overlay-note" data-job-state={status.state}>{jobProgressText(status)}</p>
      {rows.length > 0 && (
        <details className="settings-details">
          <summary>Measured on the device ({rows.length} steps)</summary>
          <table className="overlay-table">
            <thead><tr><th>Method</th><th>Time</th><th>Share</th></tr></thead>
            <tbody>
              {rows.map((row) => (
                <tr key={row.method} className={row.skipped_for_time ? 'unavailable' : undefined}>
                  <td>{row.method}</td>
                  <td>{row.skipped_for_time ? 'not started (time budget)' : `${row.ms} ms`}</td>
                  <td>{row.skipped_for_time ? '—' : `${Math.round(row.share * 100)}%`}</td>
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
