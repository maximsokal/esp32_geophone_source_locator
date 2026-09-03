import type { DeviceStatus, ObjectProfile } from '../../domain/device';

interface Props {
  status: DeviceStatus | null;
  object: ObjectProfile | null;
}

function measured(valid: boolean, text: string): string {
  return valid ? text : 'unavailable';
}

export default function SensorsFeature({ status, object }: Props) {
  const pods = status?.pods ?? [];
  return (
    <section className="console-page">
      <div className="section-heading">
        <div><span className="eyebrow">SENSOR PODS</span><h1>Four remote assemblies</h1></div>
        <p>Only values actually published by ESP are shown. nRF54L15 local/global positioning remains unavailable until that transport is connected.</p>
      </div>
      <div className="pod-grid">
        {[0, 1, 2, 3].map((index) => {
          const pod = pods[index];
          const connected = pod !== undefined && pod.state !== 'DISCONNECTED';
          return (
            <article className="console-card pod-card" key={index}>
              <div className="card-heading">
                <h2>Pod {index + 1}</h2>
                <span className={`state-pill ${connected ? 'ready' : 'offline'}`}>{pod?.state ?? 'DISCONNECTED'}</span>
              </div>
              <dl className="console-dl compact">
                <div><dt>Cable</dt><dd>{object?.cable_lengths_m[index]?.toFixed(2) ?? '5.00'} m</dd></div>
                <div><dt>Pressure</dt><dd>{pod ? measured(pod.barometer.valid, `${(pod.barometer.pressure_pa / 100).toFixed(1)} hPa`) : 'unavailable'}</dd></div>
                <div><dt>Relative height</dt><dd>{pod ? measured(pod.barometer.valid, `${(pod.barometer.relative_altitude_cm / 100).toFixed(2)} m`) : 'unavailable'}</dd></div>
                <div><dt>Temperature</dt><dd>awaiting pod environmental driver</dd></div>
                <div><dt>Roll / pitch</dt><dd>{pod ? measured(pod.imu.valid, `${pod.imu.roll_deg.toFixed(1)}° / ${pod.imu.pitch_deg.toFixed(1)}°`) : 'unavailable'}</dd></div>
                <div><dt>Yaw</dt><dd>{pod ? measured(pod.imu.valid, `${pod.imu.yaw_relative_deg.toFixed(1)}° relative`) : 'unavailable'}</dd></div>
                <div><dt>Geophone instant</dt><dd>{pod ? measured(pod.geophone.valid, `${pod.geophone.instant_uv.toFixed(2)} µV`) : 'unavailable'}</dd></div>
                <div><dt>Geophone RMS</dt><dd>{pod ? measured(pod.geophone.valid, `${pod.geophone.rms_uv.toFixed(2)} µV`) : 'unavailable'}</dd></div>
                <div><dt>Local X/Y</dt><dd>awaiting XIAO nRF54L15</dd></div>
                <div><dt>Global coordinates</dt><dd>{object?.location.valid ? 'base GPS bound; waiting for local offset' : 'base GPS not bound'}</dd></div>
              </dl>
            </article>
          );
        })}
      </div>
    </section>
  );
}
