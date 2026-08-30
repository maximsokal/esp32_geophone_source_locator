import type { SceneModel, SensorPlacement } from '../domain/scene';

interface SensorPanelProps {
  scene: SceneModel;
  selectedId: string;
  onSelect: (sensorId: string) => void;
}

function formatNumber(value: number): string {
  return `${value >= 0 ? '+' : ''}${value.toFixed(2)}`;
}

function SensorRow({
  sensor,
  selected,
  onSelect,
}: {
  sensor: SensorPlacement;
  selected: boolean;
  onSelect: (sensorId: string) => void;
}) {
  const [x, y, z] = sensor.position;

  return (
    <button
      type="button"
      className={`sensor-row${selected ? ' selected' : ''}`}
      onClick={() => onSelect(sensor.id)}
    >
      <span className={`sensor-dot ${sensor.role}`} />
      <span className="sensor-name">
        <strong>{sensor.label}</strong>
        <small>{sensor.locked ? 'POSITION LOCKED' : 'POSITION UNLOCKED'}</small>
      </span>
      <span className="sensor-coordinates">
        <span>X {formatNumber(x)}</span>
        <span>Y {formatNumber(y)}</span>
        <span>Z {formatNumber(z)} m</span>
      </span>
    </button>
  );
}

export function SensorPanel({ scene, selectedId, onSelect }: SensorPanelProps) {
  const selected = scene.sensors.find((sensor) => sensor.id === selectedId) ?? scene.sensors[0];

  return (
    <aside className="side-panel">
      <section className="panel-card">
        <div className="panel-title">Coordinate frame</div>
        <dl className="status-list">
          <div>
            <dt>Frame</dt>
            <dd>{scene.frameId}</dd>
          </div>
          <div>
            <dt>Scene source</dt>
            <dd>{scene.source.toUpperCase()}</dd>
          </div>
          <div>
            <dt>Units</dt>
            <dd>metres</dd>
          </div>
        </dl>
      </section>

      <section className="panel-card grow">
        <div className="panel-title">Device placement</div>
        <div className="sensor-list">
          {scene.sensors.map((sensor) => (
            <SensorRow
              key={sensor.id}
              sensor={sensor}
              selected={sensor.id === selectedId}
              onSelect={onSelect}
            />
          ))}
        </div>
      </section>

      {selected !== undefined && (
        <section className="panel-card selection-card">
          <div className="panel-title">Selected · {selected.label}</div>
          <div className="selection-grid">
            <span>Role</span>
            <strong>{selected.role === 'main' ? 'MAIN DEVICE' : 'REMOTE GEOPHONE'}</strong>
            <span>Lock</span>
            <strong>{selected.locked ? 'LOCKED' : 'UNLOCKED'}</strong>
            <span>Uncertainty</span>
            <strong>
              {selected.uncertaintyM === undefined
                ? '--'
                : `±${selected.uncertaintyM[0].toFixed(2)} / ±${selected.uncertaintyM[1].toFixed(2)} / ±${selected.uncertaintyM[2].toFixed(2)} m`}
            </strong>
          </div>
        </section>
      )}
    </aside>
  );
}
