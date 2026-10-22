import { useEffect, useRef, useState } from 'react';
import './workflow.css';
import type { DeviceStatus, WorkflowCalibrationNode } from '../../domain/device';
import { capturePhoneLocation, type WorkflowCommandResult } from '../../api/deviceApi';
import type { PhoneLocationInput } from '../../domain/device';
import { useLocale, useT, type I18nKey } from '../../i18n';
import { formatDistance, formatNumber } from '../../i18n/format';
import MaterialSettingsForm from '../objects/MaterialSettingsForm';
import WorkflowResultViewer from './WorkflowResultViewer';
import {
  rangingUncertaintyMm,
  resultFailureKey,
  workflowNumberText,
  workflowViewModel,
  WORKFLOW_UNKNOWN,
  type WorkflowBodySection,
  type WorkflowViewAction,
} from './workflowViewModel';

interface Props {
  status: DeviceStatus | null;
  objectName: string | null;
  sendCommand: (command: string, generation: number, location?: PhoneLocationInput) => Promise<WorkflowCommandResult>;
  onRefreshStatus: () => Promise<boolean>;
  onWorkflowUpdate: (workflow: NonNullable<DeviceStatus['workflow']>) => void;
  onOpenScanner: (objectId: string, sessionId: string | null) => void;
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
  if (normalized === 'insufficient_nodes') return t('workflow.ranging.reason.insufficient_nodes');
  const pair = normalized.match(/^pair_failed:(.+)$/);
  if (pair !== null) return t('workflow.ranging.reason.pair_failed', { pair: pair[1] });
  const node = normalized.match(/^geometry_unsolved:(.+)$/);
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
        <div><dt>{t('workflow.calibration.relative_z')}</dt><dd>{mmText(node.height?.relative_z_mm, locale)}</dd></div>
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
  return (
    <div className="workflow-node-list">
      {value.per_node.map((node, index) => (
        <article className="workflow-node" key={`${node.node}-${index}`}>
          <h3>{node.node || WORKFLOW_UNKNOWN}</h3>
          <dl className="console-dl">
            <div><dt>{t('workflow.placement.vertical')}</dt><dd>{yesNo(node.vertical, t)}</dd></div>
            <div><dt>{t('workflow.placement.height_rel_main')}</dt><dd>{mmText(node.height_rel_main_mm, locale)}</dd></div>
            <div><dt>{t('workflow.placement.altitude_asl')}</dt><dd>{node.altitude_asl_m === null ? WORKFLOW_UNKNOWN : `${workflowNumberText(node.altitude_asl_m, locale)} m`}</dd></div>
            <div><dt>{t('workflow.placement.stable')}</dt><dd>{yesNo(node.stable, t)}</dd></div>
          </dl>
        </article>
      ))}
    </div>
  );
}

function RangingSection({ value, workflowReason }: {
  value: Extract<WorkflowBodySection, { kind: 'ranging' }>['value'];
  workflowReason: string | null | undefined;
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
      <p>{t('workflow.ranging.pairs')}: {workflowNumberText(done, locale)} / {workflowNumberText(total, locale)}</p>
      <progress max={100} value={percent ?? undefined} aria-label={t('workflow.ranging.pairs')} />
      <p>{t('workflow.ranging.residual')}: {mmText(residual, locale)}</p>
      {failureReason !== '' && <p role="alert">{t('workflow.ranging.failure_reason')}: {rangingReasonLabel(failureReason, t)}</p>}
      {(geometry?.nodes ?? []).map((node, index) => {
        const uncertainty = rangingUncertaintyMm(node.uncertainty_mm);
        return (
          <p key={`${node.node_id}-${index}`}>
            {t('workflow.ranging.remote_sigma', { number: workflowNumberText(node.node_id, locale) })}:{' '}
            {uncertainty === null ? WORKFLOW_UNKNOWN : mmText(uncertainty, locale)}
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

function WorkflowSection({ section, status }: {
  section: WorkflowBodySection;
  status: DeviceStatus | null;
}) {
  const { locale } = useLocale();
  const t = useT();
  switch (section.kind) {
    case 'object': return <p className="workflow-object-name">{section.name || WORKFLOW_UNKNOWN}</p>;
    case 'sensor_check': return (
      <div>
        <p>{t('workflow.sensor_check.connected')}: {workflowNumberText(section.connectedRemotes, locale)}</p>
        <p>{section.messageKey === null ? WORKFLOW_UNKNOWN : t(section.messageKey)}</p>
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
    case 'layout': return <p role="status">{t(section.value.placement.all_stable ? 'workflow.layout.ready' : 'workflow.layout.waiting')}</p>;
    case 'ranging': return (
      <>
        {status?.workflow?.layout_result !== null && status?.workflow?.layout_result !== undefined && (
          <p role="status">{t(status.workflow.layout_result ? 'workflow.layout.solved' : 'workflow.layout.unsolved')}</p>
        )}
        <RangingSection value={section.value} workflowReason={status?.workflow?.layout_reason} />
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
      disabled={pending}
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
}: Props) {
  const t = useT();
  const workflow = status?.workflow;
  const model = workflowViewModel(status, objectName);
  const [pendingCommand, setPendingCommand] = useState<string | null>(null);
  const [deleteArmed, setDeleteArmed] = useState(false);
  const [notice, setNotice] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const gpsCaptureGenerationRef = useRef<number | null>(null);

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
          <WorkflowSection key={`${section.kind}-${index}`} section={section} status={status} />
        ))}
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
