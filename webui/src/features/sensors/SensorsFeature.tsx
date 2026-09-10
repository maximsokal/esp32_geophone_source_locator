import type { DeviceStatus, ObjectProfile } from '../../domain/device';

interface Props {
  status: DeviceStatus | null;
  object: ObjectProfile | null;
  error: string | null;
}

function measured(valid: boolean, text: string): string {
  return valid ? text : 'unavailable';
}

export default function SensorsFeature({ status, object, error }: Props) {
  const pods = status?.pods ?? [];
  const acquisition = status?.acquisition;
  const serial = acquisition?.serial;
  const streamStuck = serial?.stream_stuck === true || serial?.stream_state === 'STUCK';
  const serialInterference = serial?.suspicious === true || serial?.stream_state === 'SUSPICIOUS';
  const serialStream = serial === undefined
    ? 'not observed'
    : streamStuck
      ? `ADC DATA STUCK (${serial.pattern})`
      : serialInterference
        ? `SERIAL INTERFERENCE (${serial.pattern})`
        : serial.pattern;

  return (
    <section className="console-page">
      {error !== null && <div className="console-alert" role="alert">ESP API: {error}</div>}

      <div className="section-heading">
        <div><span className="eyebrow">HARDWARE DIAGNOSTICS</span><h1>Acquisition + four remote assemblies</h1></div>
        <p>READY is only reported from explicit hardware evidence. Missing ADC/pods remain unavailable instead of being inferred from configured drivers.</p>
      </div>

      <article className="console-card wide-card">
        <div className="card-heading">
          <div><span className="eyebrow">BASE ADC</span><h2>{acquisition?.source ?? 'Acquisition source'}</h2></div>
          <span className={`state-pill ${acquisition?.hardware_ready ? 'ready' : 'offline'}`}>
            {acquisition?.hardware_ready ? 'DETECTED' : 'NOT DETECTED'}
          </span>
        </div>
        <dl className="console-dl">
          <div><dt>Hardware proof</dt><dd>{acquisition?.hardware_ready ? 'BUSY pulse train verified' : 'no verified conversion pulse train'}</dd></div>
          <div><dt>Acquisition</dt><dd>{acquisition?.running ? 'running' : acquisition?.requested_running ? 'requested but unhealthy' : 'stopped'}</dd></div>
          <div><dt>Last error</dt><dd>{acquisition?.last_error_name && acquisition.last_error_name !== 'ESP_OK' ? acquisition.last_error_name : 'none'}</dd></div>
          <div><dt>Sample rate</dt><dd>{acquisition?.sample_rate_hz ? `${acquisition.sample_rate_hz} Hz` : '—'}</dd></div>
          <div><dt>Serial stream</dt><dd>{serialStream}</dd></div>
          <div><dt>Stream state</dt><dd>{serial?.stream_state ?? 'not observed'}</dd></div>
          <div><dt>Serial quality</dt><dd>{serial?.quality ?? 'not observed'}</dd></div>
          <div><dt>Signal quality</dt><dd>{acquisition?.signal?.quality ?? 'not observed'}</dd></div>
          <div><dt>Serial streak</dt><dd>{serial ? `${serial.suspicious_streak} abnormal · ${serial.busy_level} BUSY` : 'not observed'}</dd></div>
        </dl>
      </article>

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
                <div><dt>Temperature</dt><dd>{pod ? measured(pod.environment.temperature_valid, `${pod.environment.temperature_c.toFixed(1)} °C`) : 'unavailable'}</dd></div>
                <div><dt>Humidity</dt><dd>{pod ? measured(pod.environment.humidity_valid, `${pod.environment.humidity_percent.toFixed(1)} %`) : 'unavailable'}</dd></div>
                <div><dt>Roll / pitch</dt><dd>{pod ? measured(pod.imu.valid, `${pod.imu.roll_deg.toFixed(1)}° / ${pod.imu.pitch_deg.toFixed(1)}°`) : 'unavailable'}</dd></div>
                <div><dt>Yaw</dt><dd>{pod ? measured(pod.imu.valid, `${pod.imu.yaw_relative_deg.toFixed(1)}° relative`) : 'unavailable'}</dd></div>
                <div><dt>Geophone instant</dt><dd>{pod ? measured(pod.geophone.valid, `${pod.geophone.instant_uv.toFixed(2)} µV`) : 'unavailable'}</dd></div>
                <div><dt>Geophone RMS</dt><dd>{pod ? measured(pod.geophone.valid, `${pod.geophone.rms_uv.toFixed(2)} µV`) : 'unavailable'}</dd></div>
                <div><dt>Local X/Y</dt><dd>{pod ? measured(pod.position.local_valid, `${pod.position.local_x_m.toFixed(2)} / ${pod.position.local_y_m.toFixed(2)} m`) : 'unavailable'}</dd></div>
                <div><dt>Global coordinates</dt><dd>{pod?.position.global_valid
                  ? `${pod.position.latitude.toFixed(6)}, ${pod.position.longitude.toFixed(6)}${pod.position.altitude_valid ? ` / ${pod.position.altitude_m.toFixed(1)} m` : ''}`
                  : object?.location.valid
                    ? 'base GPS bound; pod position unavailable'
                    : 'unavailable'}</dd></div>
              </dl>
            </article>
          );
        })}
      </div>
    </section>
  );
}
