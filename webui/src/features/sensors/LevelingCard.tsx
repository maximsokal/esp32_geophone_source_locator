import type { DeviceMainStatus, DevicePod, MainHeightReference, PodHeightReference, RecordingGateStatus } from '../../domain/device';

interface Props {
  gate: RecordingGateStatus | undefined;
  pods: DevicePod[];
  main?: DeviceMainStatus;
  onCalibrateHeights: () => Promise<boolean>;
}

const STATE_LABEL: Record<string, string> = {
  level: 'LEVEL',
  tilted: 'TILTED',
  moving: 'MOVING',
  unavailable: 'NO IMU',
  no_data: 'NO DATA',
};

const HEIGHT_REFERENCE_LABEL: Record<PodHeightReference['state'], string> = {
  not_established: 'Not established',
  valid: 'Valid',
  rejected_moving: 'Rejected: node was moving',
  sensor_invalid: 'Unavailable: sensor invalid',
};

const MAIN_HEIGHT_REFERENCE_LABEL: Record<MainHeightReference['state'], string> = {
  not_established: 'Not established',
  valid: 'Valid',
  rejected_unstable: 'Rejected: pressure was unstable',
  sensor_invalid: 'Unavailable: sensor invalid',
};

/**
 * MAX-114: per-node levelling behind the recording gate, so the operator
 * sees which geophone blocks Record and by how much it is tilted.
 */
export default function LevelingCard({ gate, pods, main, onCalibrateHeights }: Props) {
  const nodes = gate?.nodes ?? [];
  const mainReference = main?.height_reference;
  const generations = new Set([
    ...pods.flatMap((pod) => pod.height_reference === undefined ? [] : [pod.height_reference.generation]),
    ...(mainReference === undefined ? [] : [mainReference.generation]),
  ]);
  return (
    <article className="console-card wide-card">
      <div className="card-heading">
        <div><span className="eyebrow">RECORDING GATE</span><h2>Geophone levelling</h2></div>
        <div>
          {gate !== undefined && <span className={`state-pill ${gate.allowed ? 'ready' : 'offline'}`}>
            {gate.available ? `${gate.accepted_count ?? 0}/${gate.assigned_count ?? 0} LEVEL` : 'UNAVAILABLE'}
          </span>}
          <button type="button" onClick={() => { void onCalibrateHeights(); }}>Calibrate heights</button>
        </div>
      </div>
      <p>Place MAIN and all REMOTEs side by side, keep them still, then press.</p>
      {gate?.available && gate.reason !== undefined && gate.reason !== '' && <p>{gate.reason}</p>}
      {gate !== undefined && !gate.available && <p>Topology not available{gate.error ? `: ${gate.error}` : ''}.</p>}
      {generations.size > 1 && <p role="status">Height references have different generations.</p>}
      {(pods.length > 0 || main !== undefined) && (
        <table className="leveling-table">
          <thead><tr><th>Node</th><th>Height reference</th><th>Relative Z</th></tr></thead>
          <tbody>
            <tr>
              <td>MAIN</td>
              <td>{mainReference === undefined
                ? 'Not reported by firmware'
                : MAIN_HEIGHT_REFERENCE_LABEL[mainReference.state]}</td>
              <td>{mainReference?.state === 'valid'
                ? `${mainReference.relative_z_mm.toFixed(1)} mm`
                : 'вЂ”'}</td>
            </tr>
            {pods.map((pod, index) => (
              <tr key={pod.id}>
                <td>Pod {index + 1}</td>
                <td>{pod.height_reference === undefined
                  ? 'Not reported by firmware'
                  : HEIGHT_REFERENCE_LABEL[pod.height_reference.state]}</td>
                <td>{pod.height_reference?.state === 'valid'
                  ? `${pod.height_reference.relative_z_mm.toFixed(1)} mm`
                  : '—'}</td>
              </tr>
            ))}
          </tbody>
        </table>
      )}
      {nodes.length > 0 && (
        <table className="leveling-table">
          <thead><tr><th>Channel</th><th>Node</th><th>State</th><th>Tilt</th></tr></thead>
          <tbody>
            {nodes.map((node) => (
              <tr key={node.channel} className={node.state === 'level' ? 'ok' : 'blocking'}>
                <td>{node.channel + 1}</td>
                <td>{node.node_id}</td>
                <td>{STATE_LABEL[node.state] ?? node.state.toUpperCase()}</td>
                <td>{node.tilt_deg === undefined ? '—' : `${node.tilt_deg.toFixed(1)}°`}</td>
              </tr>
            ))}
          </tbody>
        </table>
      )}
    </article>
  );
}
