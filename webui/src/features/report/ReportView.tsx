import type { RecordingReport } from '../../domain/reportModel';
import { makeProjection } from '../../domain/reportModel';
import type { ProjectionAxis } from '../../domain/reportModel';
import { encodeQr, qrSvgPath } from '../../domain/qrCode';
import { qrByteCapacity } from '../../domain/qrCode';
import { describeFrames } from '../../domain/spatialFrames';
import { useLocale, useT, type I18nKey } from '../../i18n';
import { formatDate, formatDistance, formatNumber } from '../../i18n/format';

/*
 * MAX-150: printable report of one saved recording. Pure view (no network,
 * no WebGL), rendered to static HTML for the print window and in tests.
 */

const PANEL_W = 320;
const PANEL_H = 220;

function QrImage({ text, label }: { text: string; label: string }) {
  const t = useT();
  const code = encodeQr(text);
  if (code === null) return <p className="report-note">{t('report.qr_too_long', { label })}</p>;
  const size = code.size + 8;
  return (
    <figure className="report-qr">
      <svg viewBox={`0 0 ${size} ${size}`} width="120" height="120" role="img" aria-label={label} shapeRendering="crispEdges">
        <rect width={size} height={size} fill="#fff" />
        <path d={qrSvgPath(code, 4)} fill="#000" />
      </svg>
      <figcaption>{label}</figcaption>
    </figure>
  );
}

interface PanelProps {
  report: RecordingReport;
  horizontal: ProjectionAxis;
  vertical: ProjectionAxis;
  title: string;
  withLocalization: boolean;
}

function GeometryPanel({ report, horizontal, vertical, title, withLocalization }: PanelProps) {
  const sensors = report.overlay.sensors.map((s) => ({ x: s.position.x_m, y: s.position.y_m, z: s.position.z_m }));
  const sectors = withLocalization ? report.overlay.sectors : [];
  const projection = makeProjection(
    [...sensors, ...sectors.map((s) => ({ x: s.position.x_m, y: s.position.y_m, z: s.position.z_m, r: s.radiusM }))],
    horizontal, vertical, PANEL_W, PANEL_H,
  );
  return (
    <figure className="report-panel">
      <svg viewBox={`0 0 ${PANEL_W} ${PANEL_H}`} width={PANEL_W} height={PANEL_H} role="img" aria-label={title}>
        <rect width={PANEL_W} height={PANEL_H} fill="#fff" stroke="#999" />
        {/* Selected last so it is drawn on top. */}
        {[...sectors].sort((a) => (a.emphasis === 'selected' ? 1 : -1)).map((sector) => {
          const c = projection.map(sector.position.x_m, sector.position.y_m, sector.position.z_m);
          const selected = sector.emphasis === 'selected';
          return (
            <g key={sector.method}>
              <circle cx={c.u} cy={c.v} r={Math.max(2, sector.radiusM * projection.scale)}
                fill={sector.color} fillOpacity={selected ? 0.45 : 0.12}
                stroke={sector.color} strokeOpacity={selected ? 1 : 0.5} strokeWidth={selected ? 2 : 1} />
              <circle cx={c.u} cy={c.v} r={2} fill={sector.color} />
            </g>
          );
        })}
        {report.overlay.sensors.map((sensor) => {
          const p = projection.map(sensor.position.x_m, sensor.position.y_m, sensor.position.z_m);
          return (
            <g key={sensor.pod}>
              <rect x={p.u - 4} y={p.v - 4} width={8} height={8} fill={sensor.pod === 0 ? '#111' : '#555'} />
              <text x={p.u + 6} y={p.v - 6} fontSize="10">{sensor.label}</text>
            </g>
          );
        })}
      </svg>
      <figcaption>{title}</figcaption>
    </figure>
  );
}

function ScanPanel({ report, horizontal, vertical, title }: Omit<PanelProps, 'withLocalization'>) {
  const projection = makeProjection(report.scanPoints, horizontal, vertical, PANEL_W, PANEL_H);
  return (
    <figure className="report-panel">
      <svg viewBox={`0 0 ${PANEL_W} ${PANEL_H}`} width={PANEL_W} height={PANEL_H} role="img" aria-label={title}>
        <rect width={PANEL_W} height={PANEL_H} fill="#fff" stroke="#999" />
        {report.scanPoints.map((p, i) => {
          const q = projection.map(p.x, p.y, p.z);
          return <rect key={i} x={q.u} y={q.v} width={1} height={1} fill="#334155" />;
        })}
      </svg>
      <figcaption>{title}</figcaption>
    </figure>
  );
}

function fmt(value: number, locale: 'en' | 'uk', digits = 2): string {
  return Number.isFinite(value) ? formatNumber(locale, value, { minimumFractionDigits: digits, maximumFractionDigits: digits }) : '—';
}

function sessionStatusText(status: string, tr: (key: I18nKey) => string): string {
  switch (status) {
    case 'complete': return tr('report.session_status.complete');
    case 'incomplete': return tr('report.session_status.incomplete');
    case 'error': return tr('report.session_status.error');
    case 'recovered_error': return tr('report.session_status.recovered_error');
    default: return `${status} (${tr('report.session_status.unknown')})`;
  }
}

export default function ReportView({ report }: { report: RecordingReport }) {
  const tr = useT();
  const { locale } = useLocale();
  const { object, session, evidence, overlay, links } = report;
  const location = object.location;
  return (
    <article className="report">
      <header>
        <h1>{tr('report.title', { object: object.name, session: session.session_id })}</h1>
        <p>{tr('report.metadata', { id: object.id, date: formatDate(locale, report.generatedAtMs) })}</p>
      </header>

      <section>
        <h2>{tr('report.location')}</h2>
        {links === null ? (
          <p>{tr('report.no_phone_gps')}</p>
        ) : (
          <>
            <p>
              {formatNumber(locale, location.latitude, { useGrouping: false, minimumFractionDigits: 6, maximumFractionDigits: 6 })}, {formatNumber(locale, location.longitude, { useGrouping: false, minimumFractionDigits: 6, maximumFractionDigits: 6 })} ({tr('report.translation_uncertainty', { value: formatNumber(locale, location.accuracy_m, { maximumFractionDigits: 0 }) })}, {location.source})
              {location.altitude_valid ? ` · ${tr('report.altitude', { value: formatNumber(locale, location.altitude_m, { minimumFractionDigits: 1, maximumFractionDigits: 1 }) })}` : ''}
            </p>
            <p><a href={links.osm}>{links.osm}</a><br /><a href={links.google}>{links.google}</a></p>
            <div className="report-qrs">
              <QrImage text={links.osm} label={tr('report.openstreetmap')} />
              <QrImage text={links.google} label={tr('report.google_maps')} />
            </div>
          </>
        )}
      </section>

      <section>
        <h2>{tr('report.coordinate_frames')}</h2>
        {(() => {
          const frames = describeFrames(location, report.scanPoints.length > 0, locale);
          return (
            <>
              <p>{frames.axisConvention} {tr('report.frames_heading_note')}</p>
              <table>
                <thead><tr><th>{tr('report.transform')}</th><th>{tr('report.status')}</th><th>{tr('report.source')}</th><th>{tr('report.note')}</th></tr></thead>
                <tbody>
                  {frames.transforms.map((transform) => (
                    <tr key={`${transform.from}-${transform.to}`}>
                      <td>{tr(`report.frame.${transform.from}` as 'report.frame.gps' | 'report.frame.sensor' | 'report.frame.scan' | 'report.frame.report')} -&gt; {tr(`report.frame.${transform.to}` as 'report.frame.gps' | 'report.frame.sensor' | 'report.frame.scan' | 'report.frame.report')}</td>
                      <td>{transform.status === 'translation_only' ? tr('report.translation_only') : tr(`report.frame.${transform.status}` as 'report.frame.measured' | 'report.frame.identity' | 'report.frame.unknown')}{transform.translationUncertaintyM !== null && transform.status === 'translation_only' ? ` ${tr('report.translation_uncertainty', { value: formatNumber(locale, transform.translationUncertaintyM, { maximumFractionDigits: 0 }) })}` : ''}</td>
                      <td>{transform.source}</td>
                      <td>{transform.note}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </>
          );
        })()}
      </section>

      <section>
        <h2>{tr('report.recording')}</h2>
        <table>
          <tbody>
            <tr><th>{tr('report.status')}</th><td>{sessionStatusText(session.status ?? (session.complete ? 'complete' : 'incomplete'), tr)}</td></tr>
            <tr><th>{tr('report.source')}</th><td>{session.source ?? '—'}</td></tr>
            <tr><th>{tr('report.sample_rate')}</th><td>{formatNumber(locale, session.sample_rate_hz)} Hz · {tr('report.channels', { count: formatNumber(locale, session.channel_count) })}</td></tr>
            <tr><th>{tr('report.duration')}</th><td>{session.recording_ms_available === false ? tr('report.unknown') : `${formatNumber(locale, session.recording_ms / 1000, { minimumFractionDigits: 1, maximumFractionDigits: 1 })} s`}</td></tr>
            <tr><th>{tr('report.samples_dropped')}</th><td>{formatNumber(locale, session.samples_written)} / {session.dropped_frames_available === false ? tr('report.unknown') : formatNumber(locale, session.dropped_frames)}</td></tr>
            <tr><th>{tr('report.remote_nodes')}</th><td>{evidence.remoteCount === null ? tr('report.policy.unknown') : tr('report.remote_count', { count: formatNumber(locale, evidence.remoteCount) })} · {tr('report.policy', { value: tr(`report.policy.${evidence.policy}` as 'report.policy.unknown' | 'report.policy.insufficient' | 'report.policy.planar_2d' | 'report.policy.capable_3d') })}</td></tr>
          </tbody>
        </table>
      </section>

      <section>
        <h2>{tr('report.sensor_geometry')}</h2>
        {evidence.positions.length === 0 ? <p>{tr('report.no_geometry')}</p> : (
          <table>
            <thead><tr><th>{tr('report.sensor')}</th><th>{tr('report.x_m')}</th><th>{tr('report.y_m')}</th><th>{tr('report.z_m')}</th><th>{tr('report.levelled')}</th></tr></thead>
            <tbody>
              {evidence.positions.map((p) => (
                <tr key={p.pod}><td>{p.pod === 0 ? tr('report.main_node') : tr('report.remote_node', { number: formatNumber(locale, p.pod) })}</td><td>{fmt(p.position.x_m, locale)}</td><td>{fmt(p.position.y_m, locale)}</td><td>{p.position.z_m === null ? tr('report.unknown') : fmt(p.position.z_m, locale)}</td><td>{tr(p.leveled ? 'report.yes' : 'report.no')}</td></tr>
              ))}
            </tbody>
          </table>
        )}
        <p className="report-note">
          {tr('report.worst_cs_residual')}: {evidence.worstResidualM === null ? '—' : formatDistance(locale, evidence.worstResidualM)} ·
          {tr('report.worst_position_uncertainty')}: {evidence.worstUncertaintyM === null ? '—' : formatDistance(locale, evidence.worstUncertaintyM)}
          {evidence.undeterminedPods.length > 0 ? ` · ${tr('report.undetermined_pods', { nodes: evidence.undeterminedPods.map((pod) => formatNumber(locale, pod)).join(', ') })}` : ''}
        </p>
      </section>

      <section className="report-break">
        <h2>{tr('report.projections')}</h2>
        <div className="report-panels">
          <GeometryPanel report={report} horizontal="x" vertical="y" title={tr('report.projection.top_geometry')} withLocalization={false} />
          <GeometryPanel report={report} horizontal="x" vertical="z" title={tr('report.projection.side_geometry')} withLocalization={false} />
          <GeometryPanel report={report} horizontal="x" vertical="y" title={tr('report.projection.top_localization')} withLocalization />
          <GeometryPanel report={report} horizontal="x" vertical="z" title={tr('report.projection.side_localization')} withLocalization />
        </div>
      </section>

      <section>
        <h2>{tr('report.scan')}</h2>
        {report.scanPoints.length === 0 ? <p>{tr('report.no_scan')}</p> : (
          <>
            <div className="report-panels">
              <ScanPanel report={report} horizontal="x" vertical="y" title={tr('report.projection.scan_top')} />
              <ScanPanel report={report} horizontal="x" vertical="z" title={tr('report.projection.scan_side')} />
            </div>
            <p className="report-note">{tr('report.scan_not_registered')}</p>
          </>
        )}
      </section>

      <section className="report-break">
        <h2>{tr('report.localization_methods')}</h2>
        {overlay.sectors.length === 0 && overlay.unavailable.length === 0 ? <p>{tr('report.not_calculated')}</p> : (
          <table>
            <thead><tr><th>{tr('report.method')}</th><th>{tr('report.source_xyz')}</th><th>{tr('report.radius')}</th><th>{tr('report.confidence')}</th><th>{tr('report.residual')}</th><th>{tr('report.dof')}</th></tr></thead>
            <tbody>
              {overlay.sectors.map((s) => (
                <tr key={s.method}>
                  <td>{s.method}{s.kind === 'joint_fusion' ? ` (${tr('report.joint')})` : ''}{s.emphasis === 'selected' ? ' *' : ''}{s.velocityInconsistent ? ` (${tr('report.velocity_inconsistent')})` : ''}</td>
                  <td>{fmt(s.position.x_m, locale)} / {fmt(s.position.y_m, locale)} / {fmt(s.position.z_m, locale)}</td>
                  <td>{formatDistance(locale, s.radiusM)}{s.radiusSource === 'nominal' ? ` (${tr('report.nominal')})` : s.radiusSource === 'posterior' ? ` (${tr('report.two_sd')})` : ''}</td>
                  <td>{fmt(s.confidence, locale)}</td>
                  <td>{s.rmsResidualUs === null ? '—' : `${formatNumber(locale, s.rmsResidualUs)} µs`}</td>
                  <td>{s.degreesOfFreedom === null ? '—' : formatNumber(locale, s.degreesOfFreedom)}</td>
                </tr>
              ))}
              {overlay.unavailable.map((u) => (
                <tr key={`u-${u.method}`}><td>{u.method}</td><td colSpan={5}>{tr('report.unavailable_reason', { reason: u.reason })}</td></tr>
              ))}
            </tbody>
          </table>
        )}
        <p className="report-note">{tr('report.radius_explanation')}</p>
      </section>

      <section>
        <h2>{tr('report.qr')}</h2>
        <QrImage text={report.qr.text} label={tr(report.qr.kind === 'full_payload' ? 'report.result_summary' : 'report.recording_reference')} />
        <p className="report-note">{tr(report.qr.kind === 'full_payload' ? 'report.qr_full_payload_note' : 'report.qr_reference_note', { bytes: qrByteCapacity(10) })}</p>
      </section>
    </article>
  );
}
