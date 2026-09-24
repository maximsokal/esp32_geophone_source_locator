import type { RecordingGateStatus } from '../../domain/device';

interface Props {
  gate: RecordingGateStatus | undefined;
}

const STATE_LABEL: Record<string, string> = {
  level: 'LEVEL',
  tilted: 'TILTED',
  moving: 'MOVING',
  unavailable: 'NO IMU',
  no_data: 'NO DATA',
};

/**
 * MAX-114: per-node levelling behind the recording gate, so the operator
 * sees which geophone blocks Record and by how much it is tilted.
 */
export default function LevelingCard({ gate }: Props) {
  if (gate === undefined) return null;
  const nodes = gate.nodes ?? [];
  return (
    <article className="console-card wide-card">
      <div className="card-heading">
        <div><span className="eyebrow">RECORDING GATE</span><h2>Geophone levelling</h2></div>
        <span className={`state-pill ${gate.allowed ? 'ready' : 'offline'}`}>
          {gate.available ? `${gate.accepted_count ?? 0}/${gate.assigned_count ?? 0} LEVEL` : 'UNAVAILABLE'}
        </span>
      </div>
      {gate.available && gate.reason !== undefined && gate.reason !== '' && <p>{gate.reason}</p>}
      {!gate.available && <p>Topology not available{gate.error ? `: ${gate.error}` : ''}.</p>}
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
