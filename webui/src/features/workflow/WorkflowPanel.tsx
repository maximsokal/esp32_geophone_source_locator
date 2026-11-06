import { Suspense, lazy, useEffect, useRef, useState } from 'react';
import './workflow.css';
import type { DeviceStatus, WorkflowCalibrationNode } from '../../domain/device';
import { capturePhoneLocation, type WorkflowCommandResult } from '../../api/deviceApi';
import type { PhoneLocationInput } from '../../domain/device';
import { useLocale, useT, type I18nKey } from '../../i18n';
import { formatDistance, formatNumber, formatPercent } from '../../i18n/format';
import MaterialSettingsForm from '../objects/MaterialSettingsForm';
import WorkflowResultViewer from './WorkflowResultViewer';
import ManualDistanceForm from './ManualDistanceForm';
import {
  placementBlocker,
  placementReasonKey,
  rangingPairQualityNotes,
  rangingPairDiagnostics,
  rangingPairsSummary,
  rangingUncertaintyMm,
  resultFailureKey,
  workflowNumberText,
  workflowViewModel,
  WORKFLOW_UNKNOWN,
  type WorkflowBodySection,
  type WorkflowViewAction,
} from './workflowViewModel';

const LiveSeismicPanel = lazy(() => import('../seismic/LiveSeismicPanel'));

/**
 * The live geophone signals are offered once the BLE Channel Sounding positions have been calculated, not
 * before: until then there are no positions to show, and the AD7606 would load the I2C work of calibration
 * and ranging.
 */
const LIVE_SIGNAL_STEPS = new Set(['RANGING_RESULT']);

interface Props {
  status: DeviceStatus | null;
  objectName: string | null;
  sendCommand: (command: string, generation: number, location?: PhoneLocationInput) => Promise<WorkflowCommandResult>;
  onRefreshStatus: () => Promise<boolean>;
  onWorkflowUpdate: (workflow: NonNullable<DeviceStatus['workflow']>) => void;
  onOpenScanner: (objectId: string, sessionId: string | null) => void;
  /** Starts/stops the geophone signal preview; without it the live-signal toggle is not offered. */
  onAcquisition?: (running: boolean) => Promise<void>;
  /** Opens the recordings of the object with the session this workflow just made selected. */
  onOpenRecordings?: (objectId: string, sessionId: string | null) => void;
}

const HEIGHT_STATE_KEYS: Record<string, I18nKey> = {
  valid: 'workflow.state.valid',
  pending: 'workflow.state.pending',
  rejected_moving: 'workflow.state.rejected_moving',
  sensor_invalid: 'workflow.state.sensor_invalid',
  not_applicable: 'workflow.state.not_applicable',
};

const IMU_STATE_KEYS: Record<string, I18nKey> = {
  pending: 'workflow.state.pending',
  calibrated: 'workflow.state.calibrated',
  rejected_moving: 'workflow.state.rejected_moving',
  rejected_gravity: 'workflow.state.rejected_gravity',
  sensor_invalid: 'workflow.state.sensor_invalid',
  not_applicable: 'workflow.state.not_applicable',
};

function stateLabel(value: unknown, labels: Record<string, I18nKey>, t: (key: I18nKey) => string): string {
  if (typeof value !== 'string') return WORKFLOW_UNKNOWN;
  return labels[value.trim().toLowerCase()] === undefined
    ? WORKFLOW_UNKNOWN
    : t(labels[value.trim().toLowerCase()]);
}

function mmText(value: unknown, locale: 'en' | 'uk'): string {
  const number = workflowNumberText(value, locale);
  return number === WORKFLOW_UNKNOWN ? number : `${number} mm`;
}

/**
 * Placement heights are shown in centimetres: the barometers cannot vouch for millimetres and the operator
 * asks for 0.1 m. A height is rounded to the nearest centimetre (halves away from zero, as on the TFT); an
 * uncertainty is rounded up so that a bound over its limit never reads as equal to it.
 */
function cmText(value: unknown, locale: 'en' | 'uk', rounding: 'nearest' | 'up'): string {
  if (typeof value !== 'number' || !Number.isFinite(value)) return WORKFLOW_UNKNOWN;
  const centimetres = rounding === 'up'
    ? Math.ceil(value / 10)
    : value < 0 ? -Math.round(-value / 10) : Math.round(value / 10);
  const number = workflowNumberText(centimetres || 0, locale);
  return number === WORKFLOW_UNKNOWN ? number : `${number} ${locale === 'uk' ? 'см' : 'cm'}`;
}

function yesNo(value: unknown, t: (key: I18nKey) => string): string {
  if (value === true) return t('report.yes');
  if (value === false) return t('report.no');
  return WORKFLOW_UNKNOWN;
}

const CALIBRATION_REASON_KEYS: Record<string, I18nKey> = {
  insufficient_nodes: 'workflow.calibration.reason.insufficient_nodes',
  cs_spread_too_high: 'workflow.calibration.reason.cs_spread_too_high',
  timeout: 'workflow.calibration.reason.timeout',
};

const NODE_CALIBRATION_REASON_KEYS: Record<string, I18nKey> = {
  height_rejected_moving: 'workflow.calibration.reason.height_rejected_moving',
  height_sensor_invalid: 'workflow.calibration.reason.height_sensor_invalid',
  imu_rejected_moving: 'workflow.calibration.reason.imu_rejected_moving',
  imu_rejected_gravity: 'workflow.calibration.reason.imu_rejected_gravity',
  imu_sensor_invalid: 'workflow.calibration.reason.imu_sensor_invalid',
};

function calibrationReasonLabel(reason: string, t: (key: I18nKey, params?: Readonly<Record<string, string | number>>) => string): string {
  const normalized = reason.trim();
  const key = CALIBRATION_REASON_KEYS[normalized];
  if (key !== undefined) return t(key);
  const pair = normalized.match(/^cs_pair_invalid:(.+)$/);
  if (pair !== null) return t('workflow.calibration.reason.cs_pair_invalid', { pair: pair[1] });
  const nodeReason = normalized.match(/^([^:]+):([a-z_]+)$/);
  if (nodeReason !== null) {
    const nodeKey = NODE_CALIBRATION_REASON_KEYS[nodeReason[2]];
    if (nodeKey !== undefined) return t(nodeKey, { node: nodeReason[1] });
  }
  return t('workflow.reason.unknown', { reason: normalized || WORKFLOW_UNKNOWN });
}

function rangingReasonLabel(reason: string, t: (key: I18nKey, params?: Readonly<Record<string, string | number>>) => string): string {
  const normalized = reason.trim();
  if (normalized === 'timeout') return t('workflow.ranging.reason.timeout');
  if (normalized === 'geometry_inconsistent') return t('workflow.ranging.reason.geometry_inconsistent');
  if (normalized === 'insufficient_nodes') return t('workflow.ranging.reason.insufficient_nodes');
  const pair = normalized.match(/^pair_failed:(.+)$/);
  if (pair !== null) return t('workflow.ranging.reason.pair_failed', { pair: pair[1] });
  const cable = normalized.match(/^cable_exceeded:(.+)$/);
  if (cable !== null) return t('workflow.ranging.reason.cable_exceeded', { pair: cable[1] });
  const disagree = normalized.match(/^directions_disagree:(.+)$/);
  if (disagree !== null) return t('workflow.ranging.reason.directions_disagree', { pair: disagree[1] });
  const triangle = normalized.match(/^triangle_inconsistent:(.+)$/);
  if (triangle !== null) return t('workflow.ranging.reason.triangle_inconsistent', { nodes: triangle[1] });
  const node = normalized.match(/^geometry_(?:unsolved|uncertain):(.+)$/);
  if (node !== null) return t('workflow.ranging.reason.geometry_unsolved', { node: node[1] });
  return t('workflow.reason.unknown', { reason: normalized || WORKFLOW_UNKNOWN });
}

function imuVector(values: readonly (number | null)[] | undefined, locale: 'en' | 'uk'): string {
  if (values === undefined || values.length === 0 || values.some((value) => value === null)) return WORKFLOW_UNKNOWN;
  return values.map((value) => workflowNumberText(value, locale)).join(', ');
}

function CalibrationSection({ value }: { value: Extract<WorkflowBodySection, { kind: 'calibration' }>['value'] }) {
  const { locale } = useLocale();
  const t = useT();
  return (
    <div className="workflow-calibration">
      <p>{t('workflow.calibration.phase')}: {stateLabel(value.phase, {
        pending: 'workflow.state.pending',
        calibrating: 'workflow.state.calibrating',
        complete: 'workflow.state.complete',
        failed: 'workflow.state.failed',
      }, t)}</p>
      <div className="workflow-node-list">
        {value.per_node.map((node) => <CalibrationNode key={node.node} node={node} locale={locale} />)}
      </div>
      <p>
        {t('workflow.calibration.cs_pairs')}: {workflowNumberText(value.cs.pairs_done, locale)} / {workflowNumberText(value.cs.pairs_total, locale)}
        {' · '}{t('workflow.calibration.max_spread')}: {mmText(value.cs.max_spread_mm, locale)}
      </p>
      {value.result !== null && <p>{t('workflow.calibration.result')}: {yesNo(value.result, t)}</p>}
      {value.reasons.length > 0 && (
        <div>
          <strong>{t('workflow.calibration.reasons')}</strong>
          <ul>{value.reasons.map((reason, index) => <li key={`${reason}-${index}`}>{calibrationReasonLabel(reason, t)}</li>)}</ul>
        </div>
      )}
    </div>
  );
}

function CalibrationNode({ node, locale }: { node: WorkflowCalibrationNode; locale: 'en' | 'uk' }) {
  const t = useT();
  return (
    <article className="workflow-node">
      <h3>{node.node || WORKFLOW_UNKNOWN}</h3>
      <dl className="console-dl">
        <div><dt>{t('workflow.calibration.height')}</dt><dd>{stateLabel(node.height?.state, HEIGHT_STATE_KEYS, t)}</dd></div>
        <div><dt>{t('workflow.calibration.relative_z')}</dt><dd>{cmText(node.height?.relative_z_mm, locale, 'nearest')}</dd></div>
        <div><dt>{t('workflow.calibration.imu')}</dt><dd>{stateLabel(node.imu?.state, IMU_STATE_KEYS, t)}</dd></div>
        <div><dt>{t('workflow.calibration.gyro_bias')}</dt><dd>{imuVector(node.imu?.gyro_bias_mdps, locale)} mdps</dd></div>
        <div><dt>{t('workflow.calibration.gravity')}</dt><dd>{imuVector(node.imu?.gravity_ref_mg, locale)} mg</dd></div>
        <div><dt>{t('workflow.calibration.cs_pairs_ok')}</dt><dd>{yesNo(node.cs_pairs_ok, t)}</dd></div>
      </dl>
    </article>
  );
}

function PlacementSection({ value }: { value: Extract<WorkflowBodySection, { kind: 'placement' }>['value'] }) {
  const { locale } = useLocale();
  const t = useT();
  const reasonText = (reason: string): string => {
    const key = placementReasonKey(reason);
    return key === null ? reason : t(key);
  };
  const blocker = placementBlocker(value);
  return (
    <>
      <p>{t('workflow.placement.limit', {
        max: formatNumber(locale, Math.ceil(value.max_range_mm / 10)),
        samples: formatNumber(locale, value.min_samples),
      })}</p>
      {!value.all_stable && blocker !== null && (
        <div className="console-alert warning" role="status" data-testid="placement-blocked">
          <p>{t('workflow.placement.blocked', { node: blocker.node, reason: reasonText(blocker.reason) })}</p>
          <p>{t('workflow.placement.blocked_detail', {
            node: blocker.node,
            samples: formatNumber(locale, blocker.samples),
            range: cmText(blocker.range_mm, locale, 'up'),
          })}</p>
        </div>
      )}
      <div className="workflow-node-list">
        {value.per_node.map((node, index) => (
          <article className="workflow-node" key={`${node.node}-${index}`}>
            <h3>{node.node || WORKFLOW_UNKNOWN}</h3>
            <dl className="console-dl">
              <div><dt>{t('workflow.placement.vertical')}</dt><dd>{yesNo(node.vertical, t)}</dd></div>
              <div><dt>{t('workflow.placement.height_rel_main')}</dt><dd>{cmText(node.height_rel_main_mm, locale, 'nearest')}</dd></div>
              <div><dt>{t('workflow.placement.altitude_asl')}</dt><dd>{node.altitude_asl_m === null ? WORKFLOW_UNKNOWN : `${workflowNumberText(node.altitude_asl_m, locale)} m`}</dd></div>
              <div><dt>{t('workflow.placement.stable')}</dt><dd>{yesNo(node.stable, t)}</dd></div>
              <div><dt>{t('workflow.placement.reason')}</dt><dd data-reason={node.reason}>{reasonText(node.reason)}</dd></div>
              <div><dt>{t('workflow.placement.samples')}</dt><dd>{formatNumber(locale, node.samples)}</dd></div>
              <div><dt>{t('workflow.placement.range')}</dt><dd>{cmText(node.range_mm, locale, 'up')}</dd></div>
            </dl>
          </article>
        ))}
      </div>
    </>
  );
}

function RangingSection({ value, workflowReason, workflow, onSaved, onRefresh }: {
  value: Extract<WorkflowBodySection, { kind: 'ranging' }>['value'];
  workflowReason: string | null | undefined;
  workflow: DeviceStatus['workflow'];
  onSaved: (workflow: NonNullable<DeviceStatus['workflow']>) => void;
  onRefresh: () => Promise<boolean>;
}) {
  const { locale } = useLocale();
  const t = useT();
  const progress = value?.available === true ? value.progress : undefined;
  const lastResult = value?.last_result?.available === true ? value.last_result : undefined;
  const done = progress?.pairs_done;
  const total = progress?.pairs_total;
  const percent = typeof done === 'number' && typeof total === 'number' && total > 0
    ? Math.max(0, Math.min(100, (done / total) * 100))
    : null;
  const geometry = progress?.geometry ?? lastResult?.geometry;
  const failureReason = progress?.failure_reason?.trim() || workflowReason?.trim() || '';
  const residual = progress?.solution_residual_mm ?? geometry?.residual_mm;
  return (
    <div className="workflow-ranging">
      {value?.radio === 'pending' && <p role="status" className="workflow-ranging-wifi">{t('workflow.ranging.wifi_pending')}</p>}
      {value?.radio === 'off' && <p role="status" className="workflow-ranging-wifi">{t('workflow.ranging.wifi_off')}</p>}
      {value?.radio === 'resuming' && <p role="status" className="workflow-ranging-wifi">{t('workflow.ranging.wifi_back')}</p>}
      {progress?.complete === true && <p role="status">{t(progress.partial
        ? 'workflow.ranging.partial' : progress.success ? 'workflow.ranging.success' : 'workflow.ranging.failed')}</p>}
      <p>{t('workflow.ranging.pairs')}: {workflowNumberText(done, locale)} / {workflowNumberText(total, locale)}
        {percent !== null && ` (${formatPercent(locale, percent / 100)})`}</p>
      <progress max={100} value={percent ?? undefined} aria-label={t('workflow.ranging.pairs')} />
      <p>{t('workflow.ranging.residual')}: {cmText(residual, locale, 'nearest')}</p>
      {(() => {
        const summary = rangingPairsSummary(progress?.pairs ?? lastResult?.pairs ?? []);
        return summary.total > 0 && summary.failed.length > 0 && (progress?.complete === true || lastResult !== undefined) && (
          <p role="status" className="workflow-ranging-summary" data-valid-pairs={summary.valid} data-total-pairs={summary.total}>
            {t('workflow.ranging.pairs_valid', { valid: workflowNumberText(summary.valid, locale), total: workflowNumberText(summary.total, locale) })}
            {' '}{t('workflow.ranging.pairs_failed', { pairs: summary.failed.map((pair) => `${pair.a}\u2013${pair.b}`).join(', ') })}
          </p>
        );
      })()}
      {(progress?.pairs ?? lastResult?.pairs ?? []).map((pair, index) => (
        <div key={`${pair.node_a}-${pair.node_b}-${index}`}><p>
          {t('workflow.ranging.pair_distance', { a: pair.node_a, b: pair.node_b })}:{' '}
          {pair.state === 'cable_exceeded'
            ? `${cmText(pair.distance_mm, locale, 'nearest')} \u2014 ${t('workflow.ranging.cable_exceeded')}`
            : pair.state === 'valid' && pair.distance_mm >= 0
              ? cmText(pair.distance_mm, locale, 'nearest') : t('workflow.ranging.pair_unavailable')}
          {pair.manual === true && ` (${t('workflow.distance.manual')})`}
        </p>
        {(() => {
          const diagnostics = rangingPairDiagnostics(pair);
          if (diagnostics === null) return null;
          const side = (entry: typeof diagnostics.forward): string => entry === null
            ? t('workflow.ranging.pair_diag_none')
            : t('workflow.ranging.pair_diag_side', {
              distance: cmText(entry.mm, locale, 'nearest'),
              accepted: workflowNumberText(entry.accepted, locale),
              samples: workflowNumberText(entry.samples, locale),
            });
          return (
            <>
              {diagnostics.cause !== null && (
                <p className="workflow-ranging-quality" data-pair-cause={diagnostics.cause}>
                  {t('workflow.ranging.pair_cause_label')}: {t(`workflow.ranging.pair_cause.${diagnostics.cause}` as I18nKey)}
                </p>
              )}
              {(diagnostics.forward !== null || diagnostics.reverse !== null) && (
                <p className="workflow-ranging-quality workflow-ranging-diag">
                  {t('workflow.ranging.pair_diag', {
                    a: pair.node_a, b: pair.node_b, forward: side(diagnostics.forward), reverse: side(diagnostics.reverse),
                  })}
                </p>
              )}
            </>
          );
        })()}
        {rangingPairQualityNotes(pair).map((note) => (
          <p key={note.key} className="workflow-ranging-quality">
            {note.key === 'workflow.ranging.pair_disagreement'
              ? t(note.key, { value: cmText(note.value, locale, 'nearest') })
              : note.key === 'workflow.ranging.pair_failed_attempts'
                ? t(note.key, { count: workflowNumberText(note.count, locale) })
                : t(note.key)}
          </p>
        ))}
        {workflow?.step === 'RANGING_RESULT' && progress?.complete === true &&
          workflow.allowed_actions.some(action => action.toUpperCase() === 'SET_DISTANCE') &&
          Number.isSafeInteger(progress.started_at_ms) && Number.isSafeInteger(progress.updated_at_ms) && (
          <ManualDistanceForm key={`${workflow.generation}-${progress.started_at_ms}-${progress.updated_at_ms}`}
            pair={pair} generation={workflow.generation} startedAtMs={progress.started_at_ms!}
            updatedAtMs={progress.updated_at_ms!} onSaved={onSaved} onRefresh={onRefresh} />
        )}
        </div>
      ))}
      {failureReason !== '' && <p role="alert">{t('workflow.ranging.failure_reason')}: {rangingReasonLabel(failureReason, t)}</p>}
      {(geometry?.nodes ?? []).map((node, index) => {
        const uncertainty = rangingUncertaintyMm(node.uncertainty_mm);
        return (
          <p key={`${node.node_id}-${index}`}>
            {t('workflow.ranging.remote_sigma', { number: workflowNumberText(node.node_id, locale) })}:{' '}
            {uncertainty === null ? WORKFLOW_UNKNOWN : cmText(uncertainty, locale, 'nearest')}
          </p>
        );
      })}
    </div>
  );
}

function RecordingSection({ value }: { value: Extract<WorkflowBodySection, { kind: 'recording' }>['value'] }) {
  const { locale } = useLocale();
  const t = useT();
  const recording = value?.recording;
  const state = recording?.state?.toLowerCase();
  const stateKeys: Record<string, I18nKey> = {
    idle: 'workflow.state.idle',
    recording: 'workflow.state.recording',
    stopping: 'workflow.state.stopping',
    error: 'workflow.state.failed',
  };
  const stateKey = state === undefined ? undefined : stateKeys[state];
  return (
    <div>
      <p>{t('workflow.recording.live_status')}: {stateKey === undefined ? WORKFLOW_UNKNOWN : t(stateKey)}</p>
      <p>{t('workflow.recording.samples')}: {workflowNumberText(recording?.samples_written, locale)}</p>
      {recording?.session_id && <p>{t('workflow.recording.session')}: {recording.session_id}</p>}
    </div>
  );
}

function SensorPreflight({ status }: { status: DeviceStatus | null }) {
  const t = useT();
  const { locale } = useLocale();
  const healthText = (current: boolean, valid: boolean, fault = false): string => t(
    !current ? 'workflow.preflight.unavailable'
      : fault ? 'workflow.preflight.error'
        : valid ? 'workflow.preflight.ready' : 'workflow.preflight.unavailable',
  );
  const rail = status?.power?.channels?.find((channel) => channel.id === 3);
  const adc = status?.hardware?.modules.find((module) => module.id === 'ad7606');
  return (
    <div className="workflow-node-list">
      <p>{t('workflow.preflight.main', {
        barometer: healthText(status?.main !== undefined,
          status?.main?.sensor_valid?.bmp280 === true,
          status?.main?.sensor_present?.bmp280 === true && status?.main?.sensor_valid?.bmp280 === false),
        power: healthText(status?.power?.stale !== true, status?.power?.valid === true),
        rail: rail?.valid === true && rail.stale !== true
          ? `${formatNumber(locale, rail.voltage_v, { maximumFractionDigits: 2 })} V${
            rail.overcurrent_latched === true || rail.overcurrent === 'critical'
              ? ` (${t('workflow.preflight.error')})` : ''}`
          : t('workflow.preflight.unavailable'),
        adc: healthText(adc !== undefined && adc.state !== 'unknown', adc?.state === 'present',
          adc?.state === 'fault' || adc?.state === 'absent'),
      })}</p>
      {(status?.pods ?? []).map((pod) => {
        const health = pod.sensor_health;
        const connected = pod.node_link_status !== undefined
          ? ['CONNECTED', 'READY', 'MEASURING'].includes(pod.node_link_status)
          : ['READY', 'WARNING', 'ERROR'].includes(pod.state.toUpperCase());
        const current = connected && health?.current === true;
        return <p key={pod.id}>{t('workflow.preflight.node', {
          node: pod.id,
          link: healthText(connected, connected, pod.state === 'ERROR'),
          imu: healthText(current, health?.imu_valid === true, health?.imu_fault === true),
          barometer: healthText(current, health?.barometer_valid === true, health?.barometer_fault === true),
          calibration: t(current && health?.imu_calibrated === true && pod.height_reference?.state === 'valid'
            ? 'workflow.state.calibrated' : 'workflow.preflight.uncalibrated'),
        })}</p>;
      })}
    </div>
  );
}

function WorkflowSection({ section, status, onRangingSaved, onRefresh }: {
  section: WorkflowBodySection;
  status: DeviceStatus | null;
  onRangingSaved: (workflow: NonNullable<DeviceStatus['workflow']>) => void;
  onRefresh: () => Promise<boolean>;
}) {
  const { locale } = useLocale();
  const t = useT();
  switch (section.kind) {
    case 'object': return <p className="workflow-object-name">{section.name || WORKFLOW_UNKNOWN}</p>;
    case 'sensor_check': return (
      <div>
        <p>{t('workflow.sensor_check.connected')}: {workflowNumberText(section.connectedRemotes, locale)}</p>
        <p>{section.messageKey === null ? WORKFLOW_UNKNOWN : t(section.messageKey)}</p>
        <SensorPreflight status={status} />
      </div>
    );
    case 'calib_prep': return <p>{t('workflow.calib_prep.instruction')}</p>;
    case 'calibration': return <CalibrationSection value={section.value} />;
    case 'placement': return <PlacementSection value={section.value} />;
    case 'gps': {
      const messageKey = section.value.step === 'GPS_PENDING'
        ? 'workflow.gps.waiting'
        : section.value.gps_status === 'accepted' ? 'workflow.gps.accepted'
          : section.value.gps_status === 'skipped' ? 'workflow.gps.skipped'
            : section.value.gps_status === 'failed' ? 'workflow.gps.failed'
              : 'workflow.gps.choice';
      return <p role="status">{t(messageKey)}</p>;
    }
    case 'layout': return (
      <>
        <p role="status">{t(section.value.placement.all_stable ? 'workflow.layout.ready' : 'workflow.layout.waiting')}</p>
        {section.value.placement.all_stable && <p className="workflow-layout-wifi">{t('workflow.layout.wifi_choice')}</p>}
      </>
    );
    case 'ranging': return (
      <>
        {status?.workflow?.step === 'RANGING' && <p role="status">{t('workflow.ranging.wait')}</p>}
        {status?.workflow?.layout_result !== null && status?.workflow?.layout_result !== undefined && (
          <p role="status">{t(status.workflow.layout_result ? 'workflow.layout.solved' : 'workflow.layout.unsolved')}</p>
        )}
        <RangingSection value={section.value} workflowReason={status?.workflow?.layout_reason}
          workflow={status?.workflow} onSaved={onRangingSaved} onRefresh={onRefresh} />
      </>
    );
    case 'scan': return (
      <p role="status">{t(section.scanning
        ? section.value.scan_upload_status === 'verified' ? 'workflow.scan.saved' : 'workflow.scan.phone'
        : 'workflow.scan.choice')}</p>
    );
    case 'record_ready': return <p>{t('workflow.record_ready.instruction')}</p>;
    case 'recording': return <RecordingSection value={section.value} />;
    case 'stopped': return <p>{t('workflow.stopped.instruction')}</p>;
    case 'stopping': return <p>{t('workflow.stopping.instruction')}</p>;
    case 'record_error': return <p role="alert">{t('workflow.record_error.instruction')} {section.reason}</p>;
    case 'calculating': return (
      <div>
        <p>{t('workflow.calculating.progress')}: {workflowNumberText(section.progress, locale)}{section.progress === null ? '' : '%'}</p>
        <progress max={100} value={section.progress ?? undefined} aria-label={t('workflow.calculating.progress')} />
      </div>
    );
    case 'result': return (
      <div>
        <p>{t(section.success ? 'workflow.result.success' : resultFailureKey(section.calculationClass))}</p>
        {section.reason?.trim() && <p>{t('workflow.result.reason')}: {section.reason}</p>}
        {section.success && (
          <WorkflowResultViewer
            objectId={status?.workflow?.object_id ?? null}
            sessionId={status?.workflow?.session_id ?? null}
          />
        )}
      </div>
    );
    case 'idle': return <p>{t('workflow.idle.instruction')}</p>;
    case 'unknown': return <p>{WORKFLOW_UNKNOWN}</p>;
  }
}

function ActionButton({ action, pending, deleteArmed, onClick }: {
  action: WorkflowViewAction;
  pending: boolean;
  deleteArmed: boolean;
  onClick: () => void;
}) {
  const t = useT();
  const key = action.command.toUpperCase() === 'DELETE_RECORDING' && deleteArmed
    ? 'workflow.delete_confirm.button'
    : action.labelKey;
  return (
    <button
      type="button"
      data-command={action.command}
      className={action.back || !action.primary ? 'secondary' : undefined}
      disabled={pending || action.disabled === true}
      aria-disabled={action.disabled === true ? true : undefined}
      onClick={onClick}
    >{t(key)}</button>
  );
}

export default function WorkflowPanel({
  status,
  objectName,
  sendCommand,
  onRefreshStatus,
  onWorkflowUpdate,
  onOpenScanner,
  onAcquisition,
  onOpenRecordings,
}: Props) {
  const t = useT();
  const workflow = status?.workflow;
  const model = workflowViewModel(status, objectName);
  const [pendingCommand, setPendingCommand] = useState<string | null>(null);
  const [deleteArmed, setDeleteArmed] = useState(false);
  const [notice, setNotice] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const gpsCaptureGenerationRef = useRef<number | null>(null);
  const [showLive, setShowLive] = useState(false);
  const liveStep = workflow !== undefined && LIVE_SIGNAL_STEPS.has(workflow.step);
  const showLiveRef = useRef(false);
  const onAcquisitionRef = useRef(onAcquisition);
  useEffect(() => {
    showLiveRef.current = showLive;
    onAcquisitionRef.current = onAcquisition;
  });
  // The preview belongs to the live steps: leaving them (or the panel) must release the acquisition lease,
  // otherwise the AD7606 keeps running through ranging and recording setup.
  useEffect(() => {
    if (liveStep || !showLiveRef.current) return;
    setShowLive(false);
    void onAcquisitionRef.current?.(false);
  }, [liveStep]);
  useEffect(() => () => {
    if (showLiveRef.current) void onAcquisitionRef.current?.(false);
  }, []);
  const toggleLive = async (): Promise<void> => {
    if (onAcquisition === undefined) return;
    if (showLive) {
      setShowLive(false);
      await onAcquisition(false);
      return;
    }
    setShowLive(true);
    if (status?.acquisition.running !== true) await onAcquisition(true);
  };

  useEffect(() => {
    if (!model.actions.some((action) => action.command.toUpperCase() === 'DELETE_RECORDING')) {
      setDeleteArmed(false);
    }
  }, [model.actions]);

  const handleCommandResult = (result: WorkflowCommandResult): boolean => {
    if (result.ok) {
      onWorkflowUpdate(result.workflow);
      setDeleteArmed(false);
      setNotice(null);
      setError(null);
      if (result.workflow.step === 'SCANNING' && result.workflow.object_id) {
        onOpenScanner(result.workflow.object_id, null);
      }
      return true;
    }
    if (result.reason === 'stale' || result.reason === 'not_allowed') {
      void onRefreshStatus();
      setDeleteArmed(false);
      setNotice(t('workflow.state_changed'));
    } else if (result.reason === 'unknown_outcome') {
      void onRefreshStatus();
      setDeleteArmed(false);
      setError(t('api.error_control_outcome_unknown'));
    } else if (result.reason === 'error') {
      setDeleteArmed(false);
      setError(result.message === 'invalid_response' ? t('workflow.command.invalid_response') : result.message);
    }
    return false;
  };

  const captureAndAcceptGps = async (generation: number, fix: Promise<PhoneLocationInput>): Promise<void> => {
    let location: PhoneLocationInput;
    try {
      location = await fix;
    } catch {
      try {
        const failed = await sendCommand('GPS_CAPTURE_FAILED', generation);
        if (handleCommandResult(failed)) setNotice(t('workflow.gps.failed'));
      } catch (reason) {
        setError(reason instanceof Error ? reason.message : String(reason));
      }
      return;
    }
    try {
      handleCommandResult(await sendCommand('GPS_ACCEPT', generation, location));
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    }
  };

  useEffect(() => {
    if (workflow?.step !== 'GPS_PENDING' || workflow.gps_status !== 'pending' ||
        !Number.isSafeInteger(workflow.generation) || gpsCaptureGenerationRef.current === workflow.generation) {
      return;
    }
    if (typeof navigator === 'undefined' || !('geolocation' in navigator)) return;
    gpsCaptureGenerationRef.current = workflow.generation;
    void captureAndAcceptGps(workflow.generation, capturePhoneLocation());
  }, [workflow?.generation, workflow?.gps_status, workflow?.step]);

  const runAction = async (action: WorkflowViewAction): Promise<void> => {
    if (action.command.toUpperCase() === 'DELETE_RECORDING' && !deleteArmed) {
      setDeleteArmed(true);
      return;
    }
    if (workflow === undefined || !Number.isSafeInteger(workflow.generation)) return;

    setPendingCommand(action.command);
    setError(null);
    const requestsGps = action.command.toUpperCase() === 'REQUEST_GPS';
    const gpsFix = requestsGps
      ? capturePhoneLocation()
      : undefined;
    // Observe early denial while the command is pending; the original rejection still reaches the GPS failure path.
    if (gpsFix !== undefined) void gpsFix.catch(() => undefined);
    try {
      const result = await sendCommand(action.command, workflow.generation);
      if (requestsGps && result.ok && result.workflow.step === 'GPS_PENDING') {
        const pendingGeneration = result.workflow.generation;
        gpsCaptureGenerationRef.current = pendingGeneration;
      }
      if (handleCommandResult(result) && requestsGps && result.ok && result.workflow.step === 'GPS_PENDING') {
        const pendingGeneration = result.workflow.generation;
        setPendingCommand(null);
        if (gpsFix !== undefined) void captureAndAcceptGps(pendingGeneration, gpsFix);
      }
    } catch (reason) {
      setDeleteArmed(false);
      setError(reason instanceof Error ? reason.message : String(reason));
    } finally {
      setPendingCommand(null);
    }
  };

  const showScanButton = workflow?.has_scan === true && (
    workflow.step === 'RECORD_READY' || workflow.step === 'RECORDING' ||
    (workflow.step === 'RESULT' && model.sections.some((section) => section.kind === 'result' && section.success))
  );
  const scanObjectId = workflow?.object_id ?? null;
  const scanSessionId = workflow?.session_id ?? null;

  return (
    <section className="console-page workflow-page">
      <article className="console-card">
        <div className="card-heading">
          <div>
            <span className="eyebrow">{t('workflow.eyebrow')}</span>
            <h1>{objectName?.trim() || WORKFLOW_UNKNOWN}</h1>
          </div>
          <h2>{t(model.titleKey)}</h2>
        </div>
        {notice !== null && <p className="console-alert" role="status">{notice}</p>}
        {error !== null && <p className="console-error" role="alert">{t('workflow.command_failed', { error })}</p>}
        {model.sections.map((section, index) => (
          <WorkflowSection key={`${section.kind}-${index}`} section={section} status={status}
            onRangingSaved={onWorkflowUpdate} onRefresh={onRefreshStatus} />
        ))}
        {workflow?.step === 'CALIB_RESULT' && workflow.allowed_actions.some((a) => a.toUpperCase() === 'PLACE_SENSORS') && (
          <p>{t('workflow.calib_result.next')}</p>
        )}
        {workflow?.step === 'STOPPED' && workflow.object_id && (
          <>
            {workflow.calc_settings_ready !== true && (
              <p>{t('workflow.material.choose_hint')}</p>
            )}
            <MaterialSettingsForm
              objectId={workflow.object_id}
              ready={workflow.calc_settings_ready === true}
              onSaved={onRefreshStatus}
            />
          </>
        )}
        {deleteArmed && <p role="alert">{t('workflow.delete_confirm.prompt')}</p>}
        {liveStep && onAcquisition !== undefined && showLive && (
          status?.acquisition.running === true
            ? (
              <Suspense fallback={<p>{t('workflow.live.starting')}</p>}>
                <LiveSeismicPanel />
              </Suspense>
            )
            : <p role="status">{t('workflow.live.starting')}</p>
        )}
        {liveStep && onAcquisition !== undefined && <p>{t('workflow.live.hint')}</p>}
        <div className="console-actions" aria-busy={pendingCommand !== null}>
          {model.actions.map((action) => (
            <ActionButton
              key={action.command}
              action={action}
              pending={pendingCommand !== null}
              deleteArmed={deleteArmed}
              onClick={() => { void runAction(action); }}
            />
          ))}
          {liveStep && onAcquisition !== undefined && (
            <button
              type="button"
              className="secondary"
              data-command="LIVE_SIGNALS"
              onClick={() => { void toggleLive(); }}
            >{t(showLive ? 'workflow.live.hide' : 'workflow.live.show')}</button>
          )}
          {onOpenRecordings !== undefined && workflow?.object_id &&
            (workflow.step === 'STOPPED' || workflow.step === 'RESULT') && (
            <button
              type="button"
              className="secondary"
              data-command="OPEN_RECORDINGS"
              onClick={() => { onOpenRecordings(workflow.object_id!, workflow.session_id ?? null); }}
            >{t('workflow.open_recordings')}</button>
          )}
          {showScanButton && (
            <button
              type="button"
              className="secondary"
              disabled={scanObjectId === null || scanSessionId === null}
              onClick={() => {
                if (scanObjectId !== null && scanSessionId !== null) onOpenScanner(scanObjectId, scanSessionId);
              }}
            >{t('workflow.open_3d')}</button>
          )}
        </div>
      </article>
    </section>
  );
}
