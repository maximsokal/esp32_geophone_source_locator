import type { DeviceMainStatus, DevicePod, MainHeightReference, PodHeightReference, RecordingGateStatus } from '../../domain/device';
import { useLocale, useT, type I18nKey } from '../../i18n';
import { formatNumber } from '../../i18n/format';
import { recordingGateReason } from '../objects/recordingGateUi';

interface Props {
  gate: RecordingGateStatus | undefined;
  pods: DevicePod[];
  main?: DeviceMainStatus;
  onCalibrateHeights: () => Promise<boolean>;
}

const STATE_LABEL: Record<string, I18nKey> = {
  level: 'leveling.level',
  tilted: 'leveling.tilted',
  moving: 'leveling.moving',
  unavailable: 'leveling.no_imu',
  no_data: 'leveling.no_data',
};

const HEIGHT_REFERENCE_LABEL: Record<PodHeightReference['state'], I18nKey> = {
  not_established: 'leveling.not_established',
  valid: 'leveling.valid',
  rejected_moving: 'leveling.rejected_moving',
  sensor_invalid: 'leveling.unavailable_sensor_invalid',
};

const MAIN_HEIGHT_REFERENCE_LABEL: Record<MainHeightReference['state'], I18nKey> = {
  not_established: 'leveling.not_established',
  valid: 'leveling.valid',
  rejected_unstable: 'leveling.rejected_pressure_unstable',
  sensor_invalid: 'leveling.unavailable_sensor_invalid',
};

/**
 * MAX-114: per-node levelling behind the recording gate, so the operator
 * sees which geophone blocks Record and by how much it is tilted.
 */
export default function LevelingCard({ gate, pods, main, onCalibrateHeights }: Props) {
  const t = useT();
  const { locale } = useLocale();
  const nodes = gate?.nodes ?? [];
  const mainReference = main?.height_reference;
  const generations = new Set([
    ...pods.flatMap((pod) => pod.height_reference === undefined ? [] : [pod.height_reference.generation]),
    ...(mainReference === undefined ? [] : [mainReference.generation]),
  ]);
  return (
    <article className="console-card wide-card">
      <div className="card-heading">
        <div><span className="eyebrow">{t('leveling.recording_gate')}</span><h2>{t('leveling.title')}</h2></div>
        <div className="card-actions">
          {gate !== undefined && <span className={`state-pill ${gate.allowed ? 'ready' : 'offline'}`}>
            {gate.available ? t('leveling.count_level', {
              accepted: formatNumber(locale, gate.accepted_count ?? 0),
              assigned: formatNumber(locale, gate.assigned_count ?? 0),
            }) : t('leveling.unavailable')}
          </span>}
          <button type="button" onClick={() => { void onCalibrateHeights(); }}>{t('leveling.calibrate_heights')}</button>
        </div>
      </div>
      <p>{t('leveling.instructions')}</p>
      {gate?.available && recordingGateReason(gate, t) !== null && <p>{recordingGateReason(gate, t)}</p>}
      {gate !== undefined && !gate.available && <p>{t('leveling.topology_unavailable', { error: gate.error ? t('leveling.topology_error', { error: gate.error }) : '' })}</p>}
      {generations.size > 1 && <p role="status">{t('leveling.generations_differ')}</p>}
      {(pods.length > 0 || main !== undefined) && (
        <table className="leveling-table">
          <thead><tr><th>{t('leveling.node')}</th><th>{t('leveling.height_reference')}</th><th>{t('leveling.relative_z')}</th></tr></thead>
          <tbody>
            <tr>
              <td>MAIN</td>
              <td>{mainReference === undefined
                ? t('leveling.not_reported_firmware')
                : t(MAIN_HEIGHT_REFERENCE_LABEL[mainReference.state])}</td>
              <td>{mainReference?.state === 'valid'
                ? `${formatNumber(locale, Math.round(mainReference.relative_z_mm / 10) || 0)} ${locale === 'uk' ? 'см' : 'cm'}`
                : 'вЂ”'}</td>
            </tr>
            {pods.map((pod, index) => (
              <tr key={pod.id}>
                <td>{t('sensors.pod', { number: formatNumber(locale, index + 1) })}</td>
                <td>{pod.height_reference === undefined
                  ? t('leveling.not_reported_firmware')
                  : t(HEIGHT_REFERENCE_LABEL[pod.height_reference.state])}</td>
                <td>{pod.height_reference?.state === 'valid'
                  ? `${formatNumber(locale, Math.round(pod.height_reference.relative_z_mm / 10) || 0)} ${locale === 'uk' ? 'см' : 'cm'}`
                  : '—'}</td>
              </tr>
            ))}
          </tbody>
        </table>
      )}
      {nodes.length > 0 && (
        <table className="leveling-table">
          <thead><tr><th>{t('leveling.channel')}</th><th>{t('leveling.node')}</th><th>{t('leveling.state')}</th><th>{t('leveling.tilt')}</th></tr></thead>
          <tbody>
            {nodes.map((node) => (
              <tr key={node.channel} className={node.state === 'level' ? 'ok' : 'blocking'}>
                <td>{node.channel + 1}</td>
                <td>{node.node_id}</td>
                <td>{STATE_LABEL[node.state] === undefined ? node.state : t(STATE_LABEL[node.state])}</td>
                <td>{node.tilt_deg === undefined ? '—' : `${formatNumber(locale, node.tilt_deg, { maximumFractionDigits: 1 })}°`}</td>
              </tr>
            ))}
          </tbody>
        </table>
      )}
    </article>
  );
}
