import type { DeviceStatus, ObjectStoreState } from '../../domain/device';
import { activeObject, connectedPodCount, formatCoordinate } from '../../domain/device';

interface Props {
  status: DeviceStatus | null;
  objects: ObjectStoreState | null;
  error: string | null;
  onOpenObjects: () => void;
  onOpenSensors: () => void;
  onOpenActiveObject: () => void;
  onRefresh: () => void;
}

export default function DeviceDashboard({
  status,
  objects,
  error,
  onOpenObjects,
  onOpenSensors,
  onOpenActiveObject,
  onRefresh,
}: Props) {
  const active = activeObject(objects);
  const connected = connectedPodCount(status);

  return (
    <section className="console-page dashboard-grid">
      {error !== null && <div className="console-alert" role="alert">ESP API: {error}</div>}

      <article className="console-card hero-card">
        <div className="eyebrow">ESP32 GEOLOCATOR</div>
        <h1>Rescue locator console</h1>
        <p>Device control, SD objects, sensor pods, seismic stream and phone-assisted 3D scanning from one HTTPS interface.</p>
        <div className="console-actions">
          <button onClick={onOpenObjects}>Objects</button>
          <button onClick={onOpenSensors}>Sensor pods</button>
          <button onClick={onRefresh} className="secondary">Refresh</button>
        </div>
      </article>

      <article className="console-card metric-card">
        <span>DEVICE</span><strong>{status?.system.state ?? 'CONNECTING…'}</strong>
        <small>mode {status?.system.mode ?? '—'}</small>
      </article>
      <article className="console-card metric-card">
        <span>SENSOR PODS</span><strong>{connected}/4</strong>
        <small>connected / reporting</small>
      </article>
      <article className="console-card metric-card">
        <span>ACQUISITION</span><strong>{status?.acquisition.running ? 'RUNNING' : 'STOPPED'}</strong>
        <small>{status?.acquisition.sample_rate_hz ? `${status.acquisition.sample_rate_hz} Hz` : 'no active source'}</small>
      </article>
      <article className="console-card metric-card">
        <span>OBJECTS</span><strong>{objects?.available ? objects.objects.length : '—'}</strong>
        <small>{objects?.available ? 'stored on SD' : 'object store unavailable'}</small>
      </article>

      <article className="console-card wide-card">
        <div className="card-heading">
          <div><span className="eyebrow">ACTIVE OBJECT</span><h2>{active?.name ?? 'No active object'}</h2></div>
          {active !== null && <button onClick={onOpenActiveObject}>Open</button>}
        </div>
        {active === null ? (
          <p>Create or activate an object before binding phone geometry or starting an object workflow.</p>
        ) : (
          <dl className="console-dl">
            <div><dt>ID</dt><dd>{active.id}</dd></div>
            <div><dt>Base GPS</dt><dd>{active.location.valid ? `${formatCoordinate(active.location.latitude)}, ${formatCoordinate(active.location.longitude)}` : 'not bound'}</dd></div>
            <div><dt>GPS accuracy</dt><dd>{active.location.valid ? `±${active.location.accuracy_m.toFixed(1)} m` : '—'}</dd></div>
            <div><dt>Cables</dt><dd>{active.cable_lengths_m.map((length) => `${length.toFixed(1)}m`).join(' · ')}</dd></div>
          </dl>
        )}
      </article>
    </section>
  );
}
