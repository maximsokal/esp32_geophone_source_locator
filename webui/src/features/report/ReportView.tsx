import type { RecordingReport } from '../../domain/reportModel';
import { makeProjection } from '../../domain/reportModel';
import type { ProjectionAxis } from '../../domain/reportModel';
import { encodeQr, qrSvgPath } from '../../domain/qrCode';

/*
 * MAX-150: printable report of one saved recording. Pure view (no network,
 * no WebGL), rendered to static HTML for the print window and in tests.
 */

const PANEL_W = 320;
const PANEL_H = 220;

function QrImage({ text, label }: { text: string; label: string }) {
  const code = encodeQr(text);
  if (code === null) return <p className="report-note">{label}: too long for a QR code.</p>;
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

function fmt(value: number, digits = 2): string {
  return Number.isFinite(value) ? value.toFixed(digits) : '—';
}

export default function ReportView({ report }: { report: RecordingReport }) {
  const { object, session, evidence, overlay, links } = report;
  const location = object.location;
  return (
    <article className="report">
      <header>
        <h1>{object.name} — recording {session.session_id}</h1>
        <p>Object {object.id} · generated {new Date(report.generatedAtMs).toLocaleString()} · from the recording saved on the device (no live acquisition used).</p>
      </header>

      <section>
        <h2>Location</h2>
        {links === null ? (
          <p>No phone GPS location stored for this object.</p>
        ) : (
          <>
            <p>
              {fmt(location.latitude, 6)}, {fmt(location.longitude, 6)} (±{fmt(location.accuracy_m, 0)} m, {location.source})
              {location.altitude_valid ? ` · altitude ${fmt(location.altitude_m, 1)} m` : ''}
            </p>
            <p><a href={links.osm}>{links.osm}</a><br /><a href={links.google}>{links.google}</a></p>
            <div className="report-qrs">
              <QrImage text={links.osm} label="OpenStreetMap" />
              <QrImage text={links.google} label="Google Maps" />
            </div>
          </>
        )}
      </section>

      <section>
        <h2>Recording</h2>
        <table>
          <tbody>
            <tr><th>Status</th><td>{session.status ?? (session.complete ? 'complete' : 'incomplete')}</td></tr>
            <tr><th>Source</th><td>{session.source ?? '—'}</td></tr>
            <tr><th>Sample rate</th><td>{session.sample_rate_hz} Hz · {session.channel_count} channels</td></tr>
            <tr><th>Duration</th><td>{session.recording_ms_available === false ? 'unknown' : `${fmt(session.recording_ms / 1000, 1)} s`}</td></tr>
            <tr><th>Samples / dropped</th><td>{session.samples_written} / {session.dropped_frames_available === false ? 'unknown' : session.dropped_frames}</td></tr>
            <tr><th>REMOTE nodes</th><td>{evidence.remoteSummary} · policy {evidence.policy}</td></tr>
          </tbody>
        </table>
      </section>

      <section>
        <h2>Sensor geometry and levelling (frozen with the recording)</h2>
        {evidence.positions.length === 0 ? <p>No geometry was frozen with this recording.</p> : (
          <table>
            <thead><tr><th>Sensor</th><th>x (m)</th><th>y (m)</th><th>z (m)</th><th>Levelled</th></tr></thead>
            <tbody>
              {evidence.positions.map((p) => (
                <tr key={p.label}><td>{p.label}</td><td>{fmt(p.position.x_m)}</td><td>{fmt(p.position.y_m)}</td><td>{fmt(p.position.z_m)}</td><td>{p.leveled ? 'yes' : 'no'}</td></tr>
              ))}
            </tbody>
          </table>
        )}
        <p className="report-note">
          Worst CS residual {evidence.worstResidualM === null ? '—' : `${fmt(evidence.worstResidualM)} m`} ·
          worst position uncertainty {evidence.worstUncertaintyM === null ? '—' : `${fmt(evidence.worstUncertaintyM)} m`}
          {evidence.undeterminedPods.length > 0 ? ` · undetermined: Pod ${evidence.undeterminedPods.join(', ')}` : ''}
        </p>
      </section>

      <section className="report-break">
        <h2>Projections — sensor frame (MAIN at the origin)</h2>
        <div className="report-panels">
          <GeometryPanel report={report} horizontal="x" vertical="y" title="Top (x/y), geometry only" withLocalization={false} />
          <GeometryPanel report={report} horizontal="x" vertical="z" title="Side (x/z), geometry only" withLocalization={false} />
          <GeometryPanel report={report} horizontal="x" vertical="y" title="Top (x/y), with localization" withLocalization />
          <GeometryPanel report={report} horizontal="x" vertical="z" title="Side (x/z), with localization" withLocalization />
        </div>
      </section>

      <section>
        <h2>3D scan of the object</h2>
        {report.scanPoints.length === 0 ? <p>No 3D scan saved for this recording.</p> : (
          <>
            <div className="report-panels">
              <ScanPanel report={report} horizontal="x" vertical="y" title="Scan, top" />
              <ScanPanel report={report} horizontal="x" vertical="z" title="Scan, side" />
            </div>
            <p className="report-note">Scan frame. It is not yet registered to the sensor frame (AprilTag/ArUco registration, MAX-145), so localization results are not drawn on the scan.</p>
          </>
        )}
      </section>

      <section className="report-break">
        <h2>Localization methods</h2>
        {overlay.sectors.length === 0 && overlay.unavailable.length === 0 ? <p>Not calculated.</p> : (
          <table>
            <thead><tr><th>Method</th><th>Source x / y / z (m)</th><th>Radius</th><th>Confidence</th><th>Residual</th><th>DoF</th></tr></thead>
            <tbody>
              {overlay.sectors.map((s) => (
                <tr key={s.method}>
                  <td>{s.method}{s.kind === 'joint_fusion' ? ' (joint)' : ''}{s.emphasis === 'selected' ? ' ★' : ''}</td>
                  <td>{fmt(s.position.x_m)} / {fmt(s.position.y_m)} / {fmt(s.position.z_m)}</td>
                  <td>{fmt(s.radiusM)} m{s.radiusSource === 'nominal' ? ' (nominal)' : ''}</td>
                  <td>{fmt(s.confidence)}</td>
                  <td>{s.rmsResidualUs === null ? '—' : `${fmt(s.rmsResidualUs, 0)} µs`}</td>
                  <td>{s.degreesOfFreedom ?? '—'}</td>
                </tr>
              ))}
              {overlay.unavailable.map((u) => (
                <tr key={`u-${u.method}`}><td>{u.method}</td><td colSpan={5}>unavailable: {u.reason}</td></tr>
              ))}
            </tbody>
          </table>
        )}
        <p className="report-note">Radius = RMS time residual × propagation speed (a visual extent, not a statistical confidence region). ★ = method highlighted in the projections.</p>
      </section>

      <section>
        <h2>Report QR</h2>
        <QrImage text={report.qr.text} label={report.qr.kind === 'full_payload' ? 'Result summary' : 'Recording reference'} />
        <p className="report-note">{report.qr.note}</p>
      </section>
    </article>
  );
}
