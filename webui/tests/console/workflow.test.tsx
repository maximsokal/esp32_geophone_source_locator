import { afterEach, describe, expect, it, vi } from 'vitest';
import { createElement } from 'react';
import { act, create, type ReactTestRenderer } from 'react-test-renderer';
import { renderToStaticMarkup } from 'react-dom/server';
import type { DevicePod, DeviceStatus, DeviceWorkflowStatus } from '../../src/domain/device';
import { sendWorkflowCommand } from '../../src/api/deviceApi';
import type { WorkflowCommandResult } from '../../src/api/deviceApi';
import WorkflowPanel from '../../src/features/workflow/WorkflowPanel';
import {
  rangingUncertaintyMm,
  resultFailureKey,
  workflowNumberText,
  workflowViewModel,
  WORKFLOW_UNKNOWN,
} from '../../src/features/workflow/workflowViewModel';
import { workflowStatusPollDelay } from '../../src/features/workflow/workflowPolling';
import { translate } from '../../src/i18n';

vi.mock('../../src/i18n', async (importOriginal) => {
  const actual = await importOriginal<typeof import('../../src/i18n')>();
  return {
    ...actual,
    useLocale: () => ({ locale: 'en' as const, selectedLocale: 'en' as const }),
    useT: () => (key: Parameters<typeof actual.translate>[1], params?: Parameters<typeof actual.translate>[2]) => actual.translate('en', key, params),
  };
});

const STEPS = [
  'IDLE', 'OBJECT', 'SENSOR_CHECK', 'CALIB_PREP', 'CALIBRATING', 'CALIB_RESULT',
  'PLACEMENT', 'RANGING', 'RANGING_RESULT', 'RECORD_READY', 'RECORDING', 'STOPPED',
  'CALCULATING', 'RESULT',
] as const;

const calibration = {
  generation: 1,
  phase: 'pending',
  per_node: [],
  cs: { pairs_done: 0, pairs_total: 0, max_spread_mm: null },
  result: null,
  reasons: [],
};

const placement = { per_node: [], all_stable: false };

function workflow(step: string, actions: string[] = [], extra: Partial<DeviceWorkflowStatus> = {}): DeviceWorkflowStatus {
  return {
    step,
    generation: 7,
    allowed_actions: actions,
    sensor_sufficiency: 'CAPABLE_3D',
    assigned_remotes: 4,
    reason: null,
    progress: null,
    calibration,
    placement,
    object_id: 'OBJ_0001',
    session_id: 'S000001',
    has_scan: false,
    calculation_class: 'none',
    ...extra,
  } as DeviceWorkflowStatus;
}

function statusFor(
  step: string,
  actions: string[] = [],
  remotes: number | null = 0,
  extra: Partial<DeviceWorkflowStatus> = {},
): DeviceStatus {
  const status: { workflow: DeviceWorkflowStatus; pods?: DevicePod[] } = { workflow: workflow(step, actions, extra) };
  if (remotes !== null) {
    status.pods = Array.from({ length: remotes }, (_, id) => ({ id: id + 1, state: 'CONNECTED' } as DevicePod));
  }
  return status as unknown as DeviceStatus;
}

function panelHtml(status: DeviceStatus, sendCommand = vi.fn<(...args: [string, number]) => Promise<WorkflowCommandResult>>().mockResolvedValue({ ok: true, workflow: workflow('STOPPED') })): string {
  return renderToStaticMarkup(createElement(WorkflowPanel, {
    status,
    objectName: 'Quarry',
    sendCommand,
    onRefreshStatus: async () => true,
    onWorkflowUpdate: () => undefined,
    onOpenScanner: () => undefined,
  }));
}

afterEach(() => {
  vi.useRealTimers();
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

describe('workflow view model (MAX-296)', () => {
  it.each(STEPS)('maps %s to a step title and body section', (step) => {
    const view = workflowViewModel(statusFor(step), 'Quarry');
    expect(view.titleKey).toBe(`workflow.title.${step.toLowerCase()}`);
    expect(view.sections).toHaveLength(1);
  });

  it('builds only the allowed actions in stable order and marks a primary action', () => {
    const allowed = ['BACK', 'MOVE_SENSORS', 'GO_CALIBRATION'];
    const actions = workflowViewModel(statusFor('SENSOR_CHECK', allowed)).actions;
    expect(actions.map((action) => action.command)).toEqual(['GO_CALIBRATION', 'MOVE_SENSORS', 'BACK']);
    expect(actions.map((action) => action.command).sort()).toEqual(allowed.slice().sort());
    expect(actions.find((action) => action.primary)?.command).toBe('GO_CALIBRATION');
    expect(actions.find((action) => action.back)?.command).toBe('BACK');
  });

  it.each([
    [2, 'workflow.sensor_check.insufficient'],
    [3, 'workflow.sensor_check.planar'],
    [4, 'workflow.sensor_check.spatial'],
    [5, 'workflow.sensor_check.spatial'],
  ] as const)('chooses the sensor guidance for %s connected REMOTEs', (count, messageKey) => {
    const section = workflowViewModel(statusFor('SENSOR_CHECK', [], count)).sections[0];
    expect(section).toMatchObject({ kind: 'sensor_check', connectedRemotes: count, messageKey });
  });

  it('renders an unknown step and unknown numeric values as an em dash', () => {
    const view = workflowViewModel(statusFor('FUTURE_STEP', [], null));
    expect(view.titleKey).toBe('workflow.title.unknown');
    expect(view.sections).toEqual([{ kind: 'unknown' }]);
    expect(workflowNumberText(undefined)).toBe(WORKFLOW_UNKNOWN);
    expect(workflowNumberText(null)).toBe(WORKFLOW_UNKNOWN);
    expect(workflowNumberText(Number.NaN)).toBe(WORKFLOW_UNKNOWN);
  });

  it('treats INT32_MAX ranging uncertainty as unknown', () => {
    expect(rangingUncertaintyMm(2147483647)).toBeNull();
    expect(rangingUncertaintyMm(45)).toBe(45);
  });

  it('renders unknown ranging uncertainty as an em dash', () => {
    const status = statusFor('RANGING');
    (status as unknown as { ranging: unknown }).ranging = {
      available: true,
      progress: {
        pairs_done: 1,
        pairs_total: 1,
        geometry: { nodes: [{ node_id: 2, uncertainty_mm: 2147483647 }] },
      },
    };
    const html = panelHtml(status);
    expect(html).toContain('REMOTE 2 σ: —');
    expect(html).not.toContain('2147483647');
  });

  it('renders unknown calculation progress as an indeterminate bar', () => {
    const html = panelHtml(statusFor('CALCULATING', [], 0, { progress: null }));
    expect(html).toContain('—');
    expect(html).not.toContain('value="0"');
  });

  it.each([
    ['none', true, 'workflow.result.success'],
    ['record_again', false, 'workflow.result.failure.record_again'],
    ['MOVE_SENSORS', false, 'workflow.result.failure.move_sensors'],
    ['unexpected-class', false, 'workflow.result.failure.internal'],
  ] as const)('routes RESULT class %s', (calculationClass, success, messageKey) => {
    const section = workflowViewModel(statusFor('RESULT', [], 4, { calculation_class: calculationClass })).sections[0];
    expect(section).toMatchObject({ kind: 'result', success });
    expect(resultFailureKey(calculationClass)).toBe(messageKey);
  });

  it('does not render a command button that is absent from allowed_actions', () => {
    const html = panelHtml(statusFor('RECORD_READY', ['BACK']));
    expect(html.match(/<button\b/g)).toHaveLength(1);
    expect(html).toContain(translate('en', 'workflow.action.back'));
    expect(html).not.toContain(translate('en', 'workflow.action.start_recording'));
  });

  it('shows the inline material form at STOPPED until calculation settings are ready', () => {
    const html = panelHtml(statusFor('STOPPED', [], 0, { calc_settings_ready: false }));
    expect(html).toContain(translate('en', 'workflow.material.choose_hint'));
    expect(html).toContain(translate('en', 'workflow.material.title'));
    expect(html).toContain(translate('en', 'workflow.material.choose'));
  });

  it('shows the 3D entry only when has_scan is true', () => {
    const hidden = panelHtml(statusFor('RECORD_READY', [], 4, { has_scan: false }));
    const visible = panelHtml(statusFor('RECORD_READY', [], 4, { has_scan: true }));
    expect(hidden).not.toContain(translate('en', 'workflow.open_3d'));
    expect(visible).toContain(translate('en', 'workflow.open_3d'));
  });
});

describe('workflow command API', () => {
  it('sends the command body and returns the accepted workflow', async () => {
    const next = workflow('SENSOR_CHECK');
    const fetchMock = vi.fn().mockResolvedValue(new Response(JSON.stringify({ ok: true, workflow: next }), { status: 200 }));
    vi.stubGlobal('fetch', fetchMock);

    const result = await sendWorkflowCommand('START_MEASUREMENT', 7);
    expect(result).toEqual({ ok: true, workflow: next });
    expect(JSON.parse(fetchMock.mock.calls[0][1]?.body as string)).toEqual({
      action: 'workflow_command', command: 'START_MEASUREMENT', generation: 7,
    });
  });

  it.each(['stale', 'not_allowed'] as const)('returns the typed %s conflict', async (reason) => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(new Response(JSON.stringify({ ok: false, reason }), { status: 409 })));
    expect(await sendWorkflowCommand('START_RECORDING', 7)).toEqual({ ok: false, reason });
  });

  it('returns a typed error for a network failure', async () => {
    vi.stubGlobal('fetch', vi.fn().mockRejectedValue(new Error('offline')));
    const result = await sendWorkflowCommand('START_RECORDING', 7);
    expect(result.ok).toBe(false);
    if (!result.ok && result.reason === 'error') expect(result.message).toContain('network error');
  });
});

describe('workflow operator actions', () => {
  it('refetches status and shows the state-changed notice on a stale command', async () => {
    vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
    const refresh = vi.fn().mockResolvedValue(true);
    const sendCommand = vi.fn().mockResolvedValue({ ok: false, reason: 'stale' } satisfies WorkflowCommandResult);
    let renderer!: ReactTestRenderer;
    await act(async () => {
      renderer = create(createElement(WorkflowPanel, {
        status: statusFor('OBJECT', ['START_MEASUREMENT']),
        objectName: 'Quarry',
        sendCommand,
        onRefreshStatus: refresh,
        onWorkflowUpdate: () => undefined,
        onOpenScanner: () => undefined,
      }));
    });
    const button = renderer.root.findAllByType('button').find((item) => item.children.includes(translate('en', 'workflow.action.start_measurement')));
    expect(button).toBeDefined();
    await act(async () => {
      button?.props.onClick();
      await Promise.resolve();
      await Promise.resolve();
    });
    expect(sendCommand).toHaveBeenCalledWith('START_MEASUREMENT', 7);
    expect(refresh).toHaveBeenCalledTimes(1);
    expect(renderer.root.findAllByProps({ role: 'status' }).some((item) => item.children.includes(translate('en', 'workflow.state_changed')))).toBe(true);
    act(() => renderer.unmount());
  });

  it('requires a second click before DELETE_RECORDING is sent', async () => {
    vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
    const sendCommand = vi.fn().mockResolvedValue({ ok: true, workflow: workflow('OBJECT') } satisfies WorkflowCommandResult);
    let renderer!: ReactTestRenderer;
    await act(async () => {
      renderer = create(createElement(WorkflowPanel, {
        status: statusFor('STOPPED', ['DELETE_RECORDING']),
        objectName: 'Quarry',
        sendCommand,
        onRefreshStatus: async () => true,
        onWorkflowUpdate: () => undefined,
        onOpenScanner: () => undefined,
      }));
    });
    const findDeleteButton = () => renderer.root.findAllByType('button').find((item) =>
      item.children.includes(translate('en', 'workflow.action.delete_recording')) ||
      item.children.includes(translate('en', 'workflow.delete_confirm.button')));

    act(() => { findDeleteButton()?.props.onClick(); });
    expect(sendCommand).not.toHaveBeenCalled();
    expect(findDeleteButton()?.children).toContain(translate('en', 'workflow.delete_confirm.button'));

    await act(async () => {
      findDeleteButton()?.props.onClick();
      await Promise.resolve();
      await Promise.resolve();
    });
    expect(sendCommand).toHaveBeenCalledTimes(1);
    expect(sendCommand).toHaveBeenCalledWith('DELETE_RECORDING', 7);
    act(() => renderer.unmount());
  });

  it('switches poll timers between one second and five seconds', async () => {
    vi.useFakeTimers();
    const poll = vi.fn();
    setTimeout(poll, workflowStatusPollDelay('RECORDING', true, 0));
    await vi.advanceTimersByTimeAsync(999);
    expect(poll).not.toHaveBeenCalled();
    await vi.advanceTimersByTimeAsync(1);
    expect(poll).toHaveBeenCalledTimes(1);

    setTimeout(poll, workflowStatusPollDelay('STOPPED', true, 0));
    await vi.advanceTimersByTimeAsync(4999);
    expect(poll).toHaveBeenCalledTimes(1);
    await vi.advanceTimersByTimeAsync(1);
    expect(poll).toHaveBeenCalledTimes(2);
    expect(workflowStatusPollDelay('STOPPED', false, 1)).toBe(10000);
  });
});
