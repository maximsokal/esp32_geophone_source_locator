import type { SceneModel, SensorPlacement } from '../domain/scene';
import { useLocale, useT } from '../i18n';
import { formatNumber } from '../i18n/format';

interface SensorPanelProps {
  scene: SceneModel;
  selectedId: string;
  onSelect: (sensorId: string) => void;
}

function SensorRow({
  sensor,
  selected,
  onSelect,
  locale,
}: {
  sensor: SensorPlacement;
  selected: boolean;
  onSelect: (sensorId: string) => void;
  locale: 'en' | 'uk';
}) {
  const t = useT();
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
        <small>{sensor.locked ? t('scene.panel.position_locked') : t('scene.panel.position_unlocked')}</small>
      </span>
      <span className="sensor-coordinates">
        <span>X {formatNumber(locale, x, { minimumFractionDigits: 2, maximumFractionDigits: 2, signDisplay: 'always' })}</span>
        <span>Y {formatNumber(locale, y, { minimumFractionDigits: 2, maximumFractionDigits: 2, signDisplay: 'always' })}</span>
        <span>Z {formatNumber(locale, z, { minimumFractionDigits: 2, maximumFractionDigits: 2, signDisplay: 'always' })} m</span>
      </span>
    </button>
  );
}

export function SensorPanel({ scene, selectedId, onSelect }: SensorPanelProps) {
  const { locale } = useLocale();
  const t = useT();
  const selected = scene.sensors.find((sensor) => sensor.id === selectedId) ?? scene.sensors[0];

  return (
    <aside className="side-panel">
      <section className="panel-card">
        <div className="panel-title">{t('scene.panel.coordinate_frame')}</div>
        <dl className="status-list">
          <div>
            <dt>{t('scene.panel.frame')}</dt>
            <dd>{scene.frameId}</dd>
          </div>
          <div>
            <dt>{t('scene.panel.source')}</dt>
            <dd>{scene.source.toUpperCase()}</dd>
          </div>
          <div>
            <dt>{t('scene.panel.units')}</dt>
            <dd>{t('scene.panel.meters')}</dd>
          </div>
        </dl>
      </section>

      <section className="panel-card grow">
        <div className="panel-title">{t('scene.panel.device_placement')}</div>
        <div className="sensor-list">
          {scene.sensors.map((sensor) => (
            <SensorRow
              key={sensor.id}
              sensor={sensor}
              selected={sensor.id === selectedId}
              onSelect={onSelect}
              locale={locale}
            />
          ))}
        </div>
      </section>

      {selected !== undefined && (
        <section className="panel-card selection-card">
        <div className="panel-title">{t('scene.panel.selected', { name: selected.label })}</div>
          <div className="selection-grid">
            <span>{t('scene.panel.role')}</span>
            <strong>{selected.role === 'main' ? t('scene.panel.main_device') : t('scene.panel.remote_geophone')}</strong>
            <span>{t('scene.panel.lock')}</span>
            <strong>{selected.locked ? t('scene.panel.locked') : t('scene.panel.unlocked')}</strong>
            <span>{t('scan.uncertainty')}</span>
            <strong>
              {selected.uncertaintyM === undefined
                ? '--'
                : `±${formatNumber(locale, selected.uncertaintyM[0], { minimumFractionDigits: 2, maximumFractionDigits: 2 })} / ±${formatNumber(locale, selected.uncertaintyM[1], { minimumFractionDigits: 2, maximumFractionDigits: 2 })} / ±${formatNumber(locale, selected.uncertaintyM[2], { minimumFractionDigits: 2, maximumFractionDigits: 2 })} m`}
            </strong>
          </div>
        </section>
      )}
    </aside>
  );
}
