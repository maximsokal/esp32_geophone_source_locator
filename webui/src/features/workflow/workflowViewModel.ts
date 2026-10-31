import type {
  DeviceRangingStatus,
  DeviceStatus,
  DeviceWorkflowStatus,
  WorkflowCalibrationStatus,
  WorkflowPlacementNode,
  WorkflowPlacementStatus,
} from '../../domain/device';
import { connectedPodCount } from '../../domain/device';
import { formatNumber } from '../../i18n/format';
import type { I18nKey, I18nLocale } from '../../i18n';

export const WORKFLOW_UNKNOWN = '—';
export const RANGING_UNKNOWN_UNCERTAINTY_MM = 2147483647;

export function workflowNumberText(value: unknown, locale: I18nLocale = 'en'): string {
  return typeof value === 'number' && Number.isFinite(value)
    ? formatNumber(locale, value)
    : WORKFLOW_UNKNOWN;
}

export type RangingPairQualityNote =
  | { key: 'workflow.ranging.pair_one_direction' }
  | { key: 'workflow.ranging.pair_disagreement'; value: number }
  | { key: 'workflow.ranging.pair_failed_attempts'; count: number };

/**
 * Raw per-pair evidence the device already reports. These notes explain a pair;
 * they never decide acceptance (the device's geometry gate does).
 */
export function rangingPairQualityNotes(pair: {
  state: string;
  valid_samples: number;
  attempts: number;
  single_direction: boolean;
  direction_disagreement_mm: number;
}): RangingPairQualityNote[] {
  const notes: RangingPairQualityNote[] = [];
  if (pair.state === 'valid') {
    if (pair.single_direction) notes.push({ key: 'workflow.ranging.pair_one_direction' });
    else if (Number.isFinite(pair.direction_disagreement_mm) && pair.direction_disagreement_mm > 0) {
      notes.push({ key: 'workflow.ranging.pair_disagreement', value: pair.direction_disagreement_mm });
    }
  }
  const failed = pair.attempts - pair.valid_samples;
  if (Number.isFinite(failed) && failed > 0) {
    notes.push({ key: 'workflow.ranging.pair_failed_attempts', count: failed });
  }
  return notes;
}

export function rangingUncertaintyMm(value: unknown): number | null {
  return typeof value === 'number' && Number.isFinite(value) && value !== RANGING_UNKNOWN_UNCERTAINTY_MM
    ? value
    : null;
}

export type WorkflowCalculationClass = 'none' | 'record_again' | 'move_sensors' | 'internal';

export type WorkflowBodySection =
  | { kind: 'object'; name: string }
  | { kind: 'sensor_check'; connectedRemotes: number | null; messageKey: I18nKey | null }
  | { kind: 'calib_prep' }
  | { kind: 'calibration'; value: WorkflowCalibrationStatus }
  | { kind: 'placement'; value: WorkflowPlacementStatus }
  | { kind: 'gps'; value: DeviceWorkflowStatus }
  | { kind: 'layout'; value: DeviceWorkflowStatus }
  | { kind: 'ranging'; value: DeviceRangingStatus | null }
  | { kind: 'scan'; value: DeviceWorkflowStatus; scanning: boolean }
  | { kind: 'record_ready' }
  | { kind: 'recording'; value: DeviceStatus | null }
  | { kind: 'stopped' }
  | { kind: 'calculating'; progress: number | null }
  | { kind: 'result'; success: boolean; calculationClass: WorkflowCalculationClass; reason: string | null }
  | { kind: 'idle' }
  | { kind: 'unknown' };

export interface WorkflowViewAction {
  command: string;
  labelKey: I18nKey;
  primary: boolean;
  back: boolean;
  /** Shown but not available: the firmware does not allow the command yet (see placementBlocker). */
  disabled?: boolean;
}

const PLACEMENT_REASON_KEYS: Record<string, I18nKey> = {
  stable: 'workflow.placement.reason.stable',
  collecting: 'workflow.placement.reason.collecting',
  range_exceeded: 'workflow.placement.reason.range_exceeded',
  not_vertical: 'workflow.placement.reason.not_vertical',
  device_status_busy: 'workflow.placement.reason.device_status_busy',
  main_reference: 'workflow.placement.reason.main_reference',
  main_barometer: 'workflow.placement.reason.main_barometer',
  node_stale: 'workflow.placement.reason.node_stale',
  node_environment: 'workflow.placement.reason.node_environment',
  node_reference: 'workflow.placement.reason.node_reference',
  sample_stale: 'workflow.placement.reason.sample_stale',
  overflow_blocked: 'workflow.placement.reason.overflow_blocked',
  clock_backward: 'workflow.placement.reason.clock_backward',
};

/** Localisation key for a firmware placement reason; null for a name this build does not know. */
export function placementReasonKey(reason: string): I18nKey | null {
  return PLACEMENT_REASON_KEYS[reason] ?? null;
}

/** The first node that keeps the placement from being confirmed, or null when every node is stable. */
export function placementBlocker(placement: WorkflowPlacementStatus): WorkflowPlacementNode | null {
  return placement.per_node.find((node) => !node.stable) ?? null;
}

export interface WorkflowViewModel {
  titleKey: I18nKey;
  sections: WorkflowBodySection[];
  actions: WorkflowViewAction[];
}

const ACTION_ORDER = [
  'START_MEASUREMENT',
  'GO_CALIBRATION',
  'SENSORS_PLACED',
  'CALIBRATION_DONE',
  'RETRY',
  'PLACE_SENSORS',
  'PLACEMENT_CONFIRMED',
  'REQUEST_GPS',
  'GPS_SKIP',
  'CALCULATE_POSITIONS',
  'RANGING_DONE',
  'CONFIRM',
  'START_RECORDING',
  'STOP_RECORDING',
  'CALCULATE_SOURCES',
  'DELETE_RECORDING',
  'RECORD_AGAIN',
  'SCAN_START',
  'SCAN_SKIP',
  'MOVE_SENSORS',
  'CALCULATION_DONE',
  'BACK',
];

const ACTION_LABELS: Record<string, I18nKey> = {
  START_MEASUREMENT: 'workflow.action.start_measurement',
  GO_CALIBRATION: 'workflow.action.go_calibration',
  SENSORS_PLACED: 'workflow.action.sensors_placed',
  CALIBRATION_DONE: 'workflow.action.calibration_done',
  RETRY: 'workflow.action.retry',
  PLACE_SENSORS: 'workflow.action.place_sensors',
  PLACEMENT_CONFIRMED: 'workflow.action.placement_confirmed',
  REQUEST_GPS: 'workflow.action.request_gps',
  GPS_SKIP: 'workflow.action.gps_skip',
  CALCULATE_POSITIONS: 'workflow.action.calculate_positions',
  RANGING_DONE: 'workflow.action.ranging_done',
  CONFIRM: 'workflow.action.confirm',
  START_RECORDING: 'workflow.action.start_recording',
  STOP_RECORDING: 'workflow.action.stop_recording',
  CALCULATE_SOURCES: 'workflow.action.calculate_sources',
  DELETE_RECORDING: 'workflow.action.delete_recording',
  RECORD_AGAIN: 'workflow.action.record_again',
  SCAN_START: 'workflow.action.scan_start',
  SCAN_SKIP: 'workflow.action.scan_skip',
  MOVE_SENSORS: 'workflow.action.move_sensors',
  CALCULATION_DONE: 'workflow.action.calculation_done',
  BACK: 'workflow.action.back',
};

const STEP_TITLES: Record<string, I18nKey> = {
  IDLE: 'workflow.title.idle',
  OBJECT: 'workflow.title.object',
  SENSOR_CHECK: 'workflow.title.sensor_check',
  CALIB_PREP: 'workflow.title.calib_prep',
  CALIBRATING: 'workflow.title.calibrating',
  CALIB_RESULT: 'workflow.title.calib_result',
  PLACEMENT: 'workflow.title.placement',
  GPS_CHOICE: 'workflow.title.gps_choice',
  GPS_PENDING: 'workflow.title.gps_pending',
  LAYOUT_READY: 'workflow.title.layout_ready',
  RANGING: 'workflow.title.ranging',
  RANGING_RESULT: 'workflow.title.ranging_result',
  SCAN_CHOICE: 'workflow.title.scan_choice',
  SCANNING: 'workflow.title.scanning',
  RECORD_READY: 'workflow.title.record_ready',
  RECORDING: 'workflow.title.recording',
  STOPPED: 'workflow.title.stopped',
  CALCULATING: 'workflow.title.calculating',
  RESULT: 'workflow.title.result',
};

export function normalizeWorkflowCalculationClass(value: string | null | undefined): WorkflowCalculationClass {
  switch (value?.trim().toLowerCase()) {
    case 'none': return 'none';
    case 'record_again': return 'record_again';
    case 'move_sensors': return 'move_sensors';
    case 'internal': return 'internal';
    default: return 'internal';
  }
}

export function resultFailureKey(value: string | null | undefined): I18nKey {
  switch (normalizeWorkflowCalculationClass(value)) {
    case 'record_again': return 'workflow.result.failure.record_again';
    case 'move_sensors': return 'workflow.result.failure.move_sensors';
    case 'internal': return 'workflow.result.failure.internal';
    case 'none': return 'workflow.result.success';
  }
}

function connectedRemotes(status: DeviceStatus | null, workflow: DeviceWorkflowStatus): number | null {
  if (status !== null && Array.isArray(status.pods)) return connectedPodCount(status);
  const count = workflow.assigned_remotes;
  return typeof count === 'number' && Number.isSafeInteger(count) && count >= 0 ? count : null;
}

function sensorMessageKey(count: number | null, allowedActions: readonly string[]): I18nKey | null {
  if (count === null) return null;
  if (count < 3) {
    return allowedActions.some((action) => action.toUpperCase() === 'GO_CALIBRATION')
      ? 'workflow.sensor_check.limited_calibration'
      : 'workflow.sensor_check.insufficient';
  }
  if (count === 3) return 'workflow.sensor_check.planar';
  return 'workflow.sensor_check.spatial';
}

function workflowActions(allowed: readonly string[]): WorkflowViewAction[] {
  const rank = new Map(ACTION_ORDER.map((command, index) => [command, index]));
  const sorted = allowed
    .filter(command => command.toUpperCase() !== 'SET_DISTANCE')
    .map((command, originalIndex) => ({ command, originalIndex, canonical: command.toUpperCase() }))
    .sort((a, b) => (rank.get(a.canonical) ?? ACTION_ORDER.length) - (rank.get(b.canonical) ?? ACTION_ORDER.length)
      || a.canonical.localeCompare(b.canonical)
      || a.originalIndex - b.originalIndex);
  let primaryAssigned = false;
  return sorted.map(({ command, canonical }) => {
    const back = canonical === 'BACK';
    const primary = !back && !primaryAssigned;
    if (primary) primaryAssigned = true;
    return {
      command,
      labelKey: ACTION_LABELS[canonical] ?? 'workflow.action.unknown',
      primary,
      back,
    };
  });
}

export function workflowViewModel(status: DeviceStatus | null, objectName: string | null = null): WorkflowViewModel {
  const workflow = status?.workflow;
  if (workflow === undefined) {
    return { titleKey: 'workflow.title.unknown', sections: [{ kind: 'unknown' }], actions: [] };
  }

  const titleKey = STEP_TITLES[workflow.step] ?? 'workflow.title.unknown';
  const sections: WorkflowBodySection[] = [];
  switch (workflow.step) {
    case 'IDLE': sections.push({ kind: 'idle' }); break;
    case 'OBJECT': sections.push({ kind: 'object', name: objectName?.trim() || '—' }); break;
    case 'SENSOR_CHECK': {
      const count = connectedRemotes(status, workflow);
      sections.push({
        kind: 'sensor_check',
        connectedRemotes: count,
        messageKey: sensorMessageKey(count, workflow.allowed_actions ?? []),
      });
      break;
    }
    case 'CALIB_PREP': sections.push({ kind: 'calib_prep' }); break;
    case 'CALIBRATING':
    case 'CALIB_RESULT': sections.push({ kind: 'calibration', value: workflow.calibration }); break;
    case 'PLACEMENT': sections.push({ kind: 'placement', value: workflow.placement }); break;
    case 'GPS_CHOICE':
    case 'GPS_PENDING': sections.push({ kind: 'gps', value: workflow }); break;
    case 'LAYOUT_READY': sections.push({ kind: 'layout', value: workflow }); break;
    case 'RANGING':
    case 'RANGING_RESULT': sections.push({ kind: 'ranging', value: status?.ranging ?? null }); break;
    case 'SCAN_CHOICE':
    case 'SCANNING': sections.push({ kind: 'scan', value: workflow, scanning: workflow.step === 'SCANNING' }); break;
    case 'RECORD_READY': sections.push({ kind: 'record_ready' }); break;
    case 'RECORDING': sections.push({ kind: 'recording', value: status }); break;
    case 'STOPPED': sections.push({ kind: 'stopped' }); break;
    case 'CALCULATING': sections.push({ kind: 'calculating', progress: workflow.progress }); break;
    case 'RESULT': {
      const calculationClass = normalizeWorkflowCalculationClass(workflow.calculation_class);
      sections.push({
        kind: 'result',
        success: calculationClass === 'none',
        calculationClass,
        reason: workflow.reason ?? null,
      });
      break;
    }
    default: sections.push({ kind: 'unknown' });
  }

  const allowed = workflow.allowed_actions ?? [];
  // The firmware hides "Sensors placed" until every node is stable; the operator still sees it, disabled, with the reason.
  const placementBlocked = workflow.step === 'PLACEMENT' &&
    !allowed.some((command) => command.toUpperCase() === 'PLACEMENT_CONFIRMED');
  const actions = workflowActions(placementBlocked ? [...allowed, 'PLACEMENT_CONFIRMED'] : allowed)
    .map((action) => placementBlocked && action.command === 'PLACEMENT_CONFIRMED'
      ? { ...action, disabled: true }
      : action);
  return { titleKey, sections, actions };
}
