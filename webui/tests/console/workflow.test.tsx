import { afterEach, describe, expect, it, vi } from 'vitest';
import { createElement } from 'react';
import { act, create, type ReactTestRenderer } from 'react-test-renderer';
import { renderToStaticMarkup } from 'react-dom/server';
import type { DevicePod, DeviceStatus, DeviceWorkflowStatus, PhoneLocationInput, WorkflowStep } from '../../src/domain/device';
import { sendWorkflowCommand } from '../../src/api/deviceApi';
import type { WorkflowCommandResult } from '../../src/api/deviceApi';
import WorkflowPanel from '../../src/features/workflow/WorkflowPanel';
import ManualDistanceForm from '../../src/features/workflow/ManualDistanceForm';
import type { RangingPairStatus } from '../../src/domain/device';
import {
  rangingPairQualityNotes,
  rangingUncertaintyMm,
  resultFailureKey,
  workflowNumberText,
  workflowViewModel,
  WORKFLOW_UNKNOWN,
} from '../../src/features/workflow/workflowViewModel';
import { workflowStatusPollDelay } from '../../src/features/workflow/workflowPolling';
import { translate } from '../../src/i18n';
import { en } from '../../src/i18n/catalog.en.gen';
import { uk } from '../../src/i18n/catalog.uk.gen';
import max460AllowedActions from '../fixtures/max460_workflow_allowed_actions.json';

vi.mock('../../src/i18n', async (importOriginal) => {
  const actual = await importOriginal<typeof import('../../src/i18n')>();
  return {
    ...actual,
    useLocale: () => ({ locale: 'en' as const, selectedLocale: 'en' as const }),
    useT: () => (key: Parameters<typeof actual.translate>[1], params?: Parameters<typeof actual.translate>[2]) => actual.translate('en', key, params),
  };
});

type WorkflowTitleStep = Uppercase<{
  [Key in keyof typeof en]: Key extends `workflow.title.${infer Step}` ? Step : never;
}[keyof typeof en]>;
type WorkflowStepWithoutTitle = Exclude<WorkflowStep, WorkflowTitleStep>;
const allWorkflowStepsHaveTitles: [WorkflowStepWithoutTitle] extends [never] ? true : false = true;

const WORKFLOW_STEPS: readonly WorkflowStep[] = Object.keys(en)
  .filter((key) => key.startsWith('workflow.title.') && !key.endsWith('.unknown'))
  .map((key) => key.slice('workflow.title.'.length).toUpperCase() as WorkflowStep);

const ACTIONS_BY_STEP: Record<WorkflowStep, readonly string[]> = {
  IDLE: [],
  OBJECT: ['START_MEASUREMENT', 'BACK'],
  SENSOR_CHECK: ['GO_CALIBRATION', 'BACK'],
  CALIB_PREP: ['SENSORS_PLACED', 'BACK'],
  CALIBRATING: ['BACK'],
  CALIB_RESULT: ['RETRY', 'BACK'],
  PLACEMENT: ['BACK'],
  GPS_CHOICE: ['REQUEST_GPS', 'GPS_SKIP', 'BACK'],
  GPS_PENDING: ['GPS_SKIP', 'BACK'],
  LAYOUT_READY: ['CALCULATE_POSITIONS', 'CALCULATE_POSITIONS_KEEP_WIFI', 'BACK'],
  RANGING: ['BACK'],
  RANGING_RESULT: ['RETRY', 'BACK'],
  SCAN_CHOICE: ['SCAN_START', 'SCAN_SKIP', 'BACK'],
  SCANNING: ['BACK'],
  RECORD_READY: ['START_RECORDING', 'MOVE_SENSORS', 'BACK'],
  RECORDING: ['STOP_RECORDING'],
  STOPPING: [],
  RECORD_ERROR: ['RECORD_AGAIN'],
  STOPPED: ['DELETE_RECORDING', 'RECORD_AGAIN', 'MOVE_SENSORS', 'BACK'],
  CALCULATING: ['BACK'],
  RESULT: ['MOVE_SENSORS', 'BACK'],
};

const calibration = {
  generation: 1,
  phase: 'pending',
  per_node: [],
  cs: { pairs_done: 0, pairs_total: 0, max_spread_mm: null },
  result: null,
  reasons: [],
};

const placement = { per_node: [], all_stable: false, max_range_mm: 100, min_samples: 8 };

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
    gps_status: 'none',
    layout_result: null,
    layout_reason: '',
    scan_choice: 'none',
    scan_upload_status: 'none',
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
  it.each(WORKFLOW_STEPS)('maps %s to a step title and body section', (step) => {
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

  it('explains a ranging pair from the evidence the device reports', () => {
    const base = { state: 'valid', valid_samples: 6, attempts: 6, single_direction: false, direction_disagreement_mm: 0 };
    expect(rangingPairQualityNotes(base)).toEqual([]);
    expect(rangingPairQualityNotes({ ...base, single_direction: true, valid_samples: 3, attempts: 3 }))
      .toEqual([{ key: 'workflow.ranging.pair_one_direction' }]);
    expect(rangingPairQualityNotes({ ...base, direction_disagreement_mm: 1200 }))
      .toEqual([{ key: 'workflow.ranging.pair_disagreement', value: 1200 }]);
    expect(rangingPairQualityNotes({ ...base, attempts: 8 }))
      .toEqual([{ key: 'workflow.ranging.pair_failed_attempts', count: 2 }]);
    // An unmeasured pair has no direction evidence, only its failed attempts.
    expect(rangingPairQualityNotes({ ...base, state: 'missing', valid_samples: 0, attempts: 3, direction_disagreement_mm: 900 }))
      .toEqual([{ key: 'workflow.ranging.pair_failed_attempts', count: 3 }]);
    expect(rangingPairQualityNotes({ ...base, direction_disagreement_mm: Number.NaN })).toEqual([]);
  });

  it('shows pair quality notes next to the distance without changing the distance line', () => {
    const status = statusFor('RANGING');
    (status as unknown as { ranging: unknown }).ranging = {
      available: true,
      progress: {
        pairs_done: 2,
        pairs_total: 2,
        pairs: [
          { node_a: 1, node_b: 2, state: 'valid', distance_mm: 1100, spread_mm: 300, confidence: 900,
            valid_samples: 5, attempts: 7, single_direction: false, direction_disagreement_mm: 200 },
          { node_a: 1, node_b: 3, state: 'valid', distance_mm: 2000, spread_mm: 10, confidence: 900,
            valid_samples: 3, attempts: 3, single_direction: true, direction_disagreement_mm: 0 },
        ],
      },
    };
    const html = panelHtml(status);
    expect(html).toMatch(/Nodes 1–2: 110 cm<\/p>/);   // 1100 mm
    expect(html).toContain('Forward and reverse measurements differ by 20 cm');
    expect(html).toContain('Failed attempts: 2');
    expect(html).toContain('Measured in one direction only');
    expect(html).not.toContain('differ by 0');
  });

  it('shows how far the radio ranging plan has run as a percentage', () => {
    const rangingStatus = (done: number, total: number) => {
      const status = statusFor('RANGING');
      (status as unknown as { ranging: unknown }).ranging = { available: true, progress: { pairs_done: done, pairs_total: total } };
      return status;
    };
    expect(panelHtml(rangingStatus(15, 30))).toContain('15 / 30 (50%)');
    expect(panelHtml(rangingStatus(0, 20))).toContain('0 / 20 (0%)');
    expect(panelHtml(rangingStatus(20, 20))).toContain('20 / 20 (100%)');
    // Without a total there is nothing to divide by.
    expect(panelHtml(rangingStatus(0, 0))).not.toContain('%)');
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

  // Board log 2026-11-15 20:26: G2 and G3 carried one signal, G1 and G4 stayed flat. The result says so instead of a
  // vague "record again".
  it('names the channels that were not independent sensors on a failed result', () => {
    const status = statusFor('RESULT', [], 4, {
      calculation_class: 'record_again',
      channel_health: { flat_channels: [1, 4], identical_pairs: [{ a: 2, b: 3 }] },
    });
    const section = workflowViewModel(status).sections[0];
    expect(section).toMatchObject({ kind: 'result', identicalChannels: 'G2=G3', silentChannels: 'G1 G4' });
    const html = panelHtml(status);
    expect(html).toContain('data-testid="channels-identical"');
    expect(html).toContain('G2=G3');
    expect(html).toContain('data-testid="channels-silent"');
    expect(html).toContain('G1 G4');
    expect(html).toContain(translate('en', 'workflow.result.failure.record_again'));
  });

  it('shows no channel warning when the channels are healthy, absent or malformed', () => {
    const healthy = statusFor('RESULT', [], 4, {
      calculation_class: 'record_again',
      channel_health: { flat_channels: [], identical_pairs: [] },
    });
    const older = statusFor('RESULT', [], 4, { calculation_class: 'record_again' });
    const malformed = statusFor('RESULT', [], 4, {
      calculation_class: 'record_again',
      channel_health: { flat_channels: [0, 7], identical_pairs: [{ a: 9, b: 2 }] },
    });
    for (const status of [healthy, older, malformed]) {
      const html = panelHtml(status);
      expect(html).not.toContain('channels-identical');
      expect(html).not.toContain('channels-silent');
      expect(workflowViewModel(status).sections[0]).toMatchObject({ identicalChannels: '', silentChannels: '' });
    }
  });

  it('warns about the channels on a successful result too', () => {
    const html = panelHtml(statusFor('RESULT', [], 4, {
      calculation_class: 'none',
      channel_health: { flat_channels: [3], identical_pairs: [] },
    }));
    expect(html).toContain(translate('en', 'workflow.result.success'));
    expect(html).toContain('G3');
    expect(html).toContain('channels-silent');
  });

  it('offers the calculation on the phone only once a recording is stopped', () => {
    const stopped = panelHtml(statusFor('STOPPED', ['RECORD_AGAIN'], 4, { object_id: 'OBJ_0001', session_id: 'S000001', calc_settings_ready: true }));
    expect(stopped).toContain(translate('en', 'workflow.phone.action'));
    for (const step of ['RECORD_READY', 'RECORDING', 'CALCULATING', 'RESULT']) {
      expect(panelHtml(statusFor(step, [], 4))).not.toContain(translate('en', 'workflow.phone.action'));
    }
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

  describe('a scan that was taken is never hidden behind the scan question (phone run 2026-11-21 23:05)', () => {
    const open3d = translate('en', 'workflow.open_3d');
    const sections = (html: string) => ({
      question: html.includes(translate('en', 'workflow.scan.choice')),
      have: html.includes(translate('en', 'workflow.scan.choice_have_scan')),
      viewer: html.includes(translate('en', 'workflow.scan3d.loading')),
      button: html.includes(open3d),
    });

    it('shows the saved scan and a note, not only "Start a 3D scan?", when the operator is back at the scan question', () => {
      const withScan = sections(panelHtml(statusFor('SCAN_CHOICE', ACTIONS_BY_STEP.SCAN_CHOICE, 4, { has_scan: true })));
      expect(withScan).toEqual({ question: false, have: true, viewer: true, button: true });
      const without = sections(panelHtml(statusFor('SCAN_CHOICE', ACTIONS_BY_STEP.SCAN_CHOICE, 4, { has_scan: false })));
      expect(without).toEqual({ question: true, have: false, viewer: false, button: false });
    });

    it('shows the staged scan while scanning is still the step', () => {
      expect(sections(panelHtml(statusFor('SCANNING', [], 4, { has_scan: true }))).viewer).toBe(true);
      expect(sections(panelHtml(statusFor('SCANNING', [], 4, { has_scan: false }))).viewer).toBe(false);
    });

    it.each(['SCAN_CHOICE', 'RECORD_READY'] as const)('%s: "Open 3D scan" is enabled with no session id (the scan is staged) and opens the staged scan', async (step) => {
      const status = statusFor(step, ACTIONS_BY_STEP[step], 4, { has_scan: true });
      const onOpenScanner = vi.fn();
      let renderer!: ReactTestRenderer;
      await act(async () => {
        renderer = create(createElement(WorkflowPanel, {
          status, objectName: 'Quarry', sendCommand: vi.fn(), onRefreshStatus: async () => true, onWorkflowUpdate: () => undefined, onOpenScanner,
        }));
      });
      const button = renderer.root.findAllByType('button').find((node) => node.children.join('') === open3d)!;
      expect(button.props.disabled).toBe(false);
      await act(async () => { button.props.onClick(); });
      expect(onOpenScanner).toHaveBeenCalledWith(status.workflow!.object_id, null);
      act(() => renderer.unmount());
    });
  });

  it('renders every production step from device state and only its allowed actions', async () => {
    vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
    expect(allWorkflowStepsHaveTitles).toBe(true);
    expect(WORKFLOW_STEPS.slice().sort()).toEqual(
      (Object.keys(ACTIONS_BY_STEP) as WorkflowStep[]).slice().sort(),
    );

    for (const step of WORKFLOW_STEPS) {
      const allowedActions = ACTIONS_BY_STEP[step];
      let renderer!: ReactTestRenderer;
      await act(async () => {
        renderer = create(createElement(WorkflowPanel, {
          status: statusFor(step, [...allowedActions]),
          objectName: 'Quarry',
          sendCommand: vi.fn().mockResolvedValue({ ok: true, workflow: workflow(step) }),
          onRefreshStatus: async () => true,
          onWorkflowUpdate: () => undefined,
          onOpenScanner: () => undefined,
        }));
      });

      const title = renderer.root.findAllByType('h2')[0];
      expect(title.children.join('')).toBe(
        translate('en', `workflow.title.${step.toLowerCase()}` as Parameters<typeof translate>[1]),
      );

      const actionButtons = renderer.root.findByProps({ className: 'console-actions' })
        .findAllByType('button');
      const buttons = actionButtons.map((button) => button.children.join(''));
      const expectedLabels = allowedActions.map((command) => translate(
        'en',
        `workflow.action.${command.toLowerCase()}` as Parameters<typeof translate>[1],
      ));
      if (step === 'PLACEMENT' && !(allowedActions as readonly string[]).includes('PLACEMENT_CONFIRMED')) {
        // Continue stays visible but disabled until the firmware allows it; it names why in the section above.
        const label = translate('en', 'workflow.action.placement_confirmed');
        expectedLabels.push(label);
        const blocked = actionButtons.find((button) => button.children.join('') === label);
        expect(blocked?.props.disabled).toBe(true);
      }
      expect(buttons.slice().sort()).toEqual(expectedLabels.slice().sort());
      for (const command of ['CALIBRATION_DONE', 'RANGING_DONE', 'CALCULATION_DONE']) {
        expect(buttons).not.toContain(translate(
          'en',
          `workflow.action.${command.toLowerCase()}` as Parameters<typeof translate>[1],
        ));
      }
      act(() => renderer.unmount());
    }

    const sendCommand = vi.fn().mockResolvedValue({ ok: true, workflow: workflow('SENSOR_CHECK') } satisfies WorkflowCommandResult);
    let renderer!: ReactTestRenderer;
    await act(async () => {
      renderer = create(createElement(WorkflowPanel, {
        status: statusFor('OBJECT', ACTIONS_BY_STEP.OBJECT),
        objectName: 'Quarry',
        sendCommand,
        onRefreshStatus: async () => true,
        onWorkflowUpdate: () => undefined,
        onOpenScanner: () => undefined,
      }));
    });
    expect(renderer.root.findAllByType('h2')[0].children.join('')).toBe(translate('en', 'workflow.title.object'));

    await act(async () => {
      renderer.update(createElement(WorkflowPanel, {
        status: statusFor('SENSOR_CHECK', ACTIONS_BY_STEP.SENSOR_CHECK),
        objectName: 'Quarry',
        sendCommand,
        onRefreshStatus: async () => true,
        onWorkflowUpdate: () => undefined,
        onOpenScanner: () => undefined,
      }));
    });
    expect(renderer.root.findAllByType('h2')[0].children.join('')).toBe(translate('en', 'workflow.title.sensor_check'));
    expect(sendCommand).not.toHaveBeenCalled();
    act(() => renderer.unmount());
  });

  it('test_tft_webui_allowed_actions_generation_and_reconnect_parity', async () => {
    vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
    const steps = ['PLACEMENT', 'GPS_CHOICE', 'GPS_PENDING', 'LAYOUT_READY', 'SCAN_CHOICE', 'SCANNING'] as const;

    for (const step of steps) {
      const expected = max460AllowedActions[step];
      const status = statusFor(step, [...expected], 4, {
        generation: 460,
        placement: { per_node: [], all_stable: true, max_range_mm: 100, min_samples: 8 },
      });
      const modelActions = workflowViewModel(status).actions;
      expect(modelActions.map((action) => action.command).sort()).toEqual([...expected].sort());

      let renderer!: ReactTestRenderer;
      await act(async () => {
        renderer = create(createElement(WorkflowPanel, {
          status,
          objectName: 'Quarry',
          sendCommand: vi.fn().mockResolvedValue({ ok: true, workflow: workflow(step, [...expected], { generation: 461 }) }),
          onRefreshStatus: async () => true,
          onWorkflowUpdate: () => undefined,
          onOpenScanner: () => undefined,
        }));
      });
      const buttons = renderer.root.findByProps({ className: 'console-actions' }).findAllByType('button');
      expect(buttons.map((button) => button.props['data-command']).sort()).toEqual([...expected].sort());
      expect(buttons.map((button) => button.children.join('')).sort()).toEqual(
        modelActions.map((action) => translate('en', action.labelKey)).sort(),
      );
      act(() => renderer.unmount());
    }

    const generation = 460;
    const createScanChoiceSnapshot = () => statusFor('SCAN_CHOICE', [...max460AllowedActions.SCAN_CHOICE], 4, { generation });
    const renderScanChoice = async (status: DeviceStatus): Promise<ReactTestRenderer> => {
      let renderer!: ReactTestRenderer;
      await act(async () => {
        renderer = create(createElement(WorkflowPanel, {
          status,
          objectName: 'Quarry',
          sendCommand: vi.fn().mockResolvedValue({ ok: true, workflow: workflow('SCAN_CHOICE', [...max460AllowedActions.SCAN_CHOICE], { generation }) }),
          onRefreshStatus: async () => true,
          onWorkflowUpdate: () => undefined,
          onOpenScanner: () => undefined,
        }));
      });
      return renderer;
    };
    const first = await renderScanChoice(createScanChoiceSnapshot());
    const firstStep = first.root.findAllByType('h2')[0].children.join('');
    const firstButtons = first.root.findByProps({ className: 'console-actions' })
      .findAllByType('button')
      .map((button) => ({ command: button.props['data-command'], label: button.children.join('') }));
    act(() => first.unmount());

    const reconnected = await renderScanChoice(createScanChoiceSnapshot());
    expect(reconnected.root.findAllByType('h2')[0].children.join('')).toBe(firstStep);
    expect(reconnected.root.findByProps({ className: 'console-actions' })
      .findAllByType('button')
      .map((button) => ({ command: button.props['data-command'], label: button.children.join('') }))).toEqual(firstButtons);
    act(() => reconnected.unmount());
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

  it('sends a phone GPS fix with GPS_ACCEPT and omits unavailable altitude fields', async () => {
    const next = workflow('LAYOUT_READY', ['CALCULATE_POSITIONS']);
    const fetchMock = vi.fn().mockResolvedValue(new Response(JSON.stringify({ ok: true, workflow: next }), { status: 200 }));
    vi.stubGlobal('fetch', fetchMock);
    const location: PhoneLocationInput = {
      latitude: 50.4501,
      longitude: 30.5234,
      accuracy_m: 3.2,
      altitude_m: null,
      altitude_accuracy_m: null,
      captured_at_ms: 123456789,
    };

    await sendWorkflowCommand('GPS_ACCEPT', 21, {}, location);
    expect(JSON.parse(fetchMock.mock.calls[0][1]?.body as string)).toEqual({
      action: 'workflow_command',
      command: 'GPS_ACCEPT',
      generation: 21,
      latitude: 50.4501,
      longitude: 30.5234,
      accuracy_m: 3.2,
      captured_at_ms: 123456789,
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

describe('Wi-Fi choice before the distance measurement', () => {
  it('offers both buttons, explains the choice and sends exactly the pressed command', async () => {
    vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
    const next = workflow('RANGING', ['BACK'], { generation: 31 });
    const sendCommand = vi.fn(async (_command: string, _generation: number) => ({ ok: true as const, workflow: next }));
    let renderer!: ReactTestRenderer;
    await act(async () => {
      renderer = create(createElement(WorkflowPanel, {
        status: statusFor('LAYOUT_READY', ['CALCULATE_POSITIONS', 'CALCULATE_POSITIONS_KEEP_WIFI', 'BACK'], 4,
          { generation: 30, gps_status: 'accepted', placement: { all_stable: true } as DeviceWorkflowStatus['placement'] }),
        objectName: 'Quarry',
        sendCommand,
        onRefreshStatus: async () => true,
        onWorkflowUpdate: () => undefined,
        onOpenScanner: () => undefined,
      }));
    });
    const buttons = renderer.root.findByProps({ className: 'console-actions' }).findAllByType('button');
    expect(buttons.map((button) => button.props['data-command'])).toEqual(
      ['CALCULATE_POSITIONS', 'CALCULATE_POSITIONS_KEEP_WIFI', 'BACK']);
    expect(buttons.map((button) => button.children.join('')).slice(0, 2)).toEqual([
      translate('en', 'workflow.action.calculate_positions'),
      translate('en', 'workflow.action.calculate_positions_keep_wifi'),
    ]);
    expect(translate('en', 'workflow.action.calculate_positions')).toContain('Wi-Fi off');
    expect(translate('en', 'workflow.action.calculate_positions_keep_wifi')).toContain('Wi-Fi on');
    expect(JSON.stringify(renderer.toJSON())).toContain(translate('en', 'workflow.layout.wifi_choice'));

    await act(async () => {
      buttons.find((button) => button.props['data-command'] === 'CALCULATE_POSITIONS_KEEP_WIFI')?.props.onClick();
      await Promise.resolve();
    });
    expect(sendCommand).toHaveBeenCalledTimes(1);
    expect(sendCommand.mock.calls[0].slice(0, 2)).toEqual(['CALCULATE_POSITIONS_KEEP_WIFI', 30]);
    act(() => renderer.unmount());
  });

  it('says nothing about Wi-Fi while the sensors are not stable yet', () => {
    const html = panelHtml(statusFor('LAYOUT_READY', ['BACK'], 4, { placement: { all_stable: false } as DeviceWorkflowStatus['placement'] }));
    expect(html).not.toContain(translate('en', 'workflow.layout.wifi_choice'));
  });
});

describe('workflow operator actions', () => {
  it('requests a fresh phone fix, then reports permission denial with the pending generation', async () => {
    vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
    const getCurrentPosition = vi.fn((_success: PositionCallback, failure: PositionErrorCallback) => {
      failure({ code: 1, message: 'permission denied', PERMISSION_DENIED: 1, POSITION_UNAVAILABLE: 2, TIMEOUT: 3 } as GeolocationPositionError);
    });
    vi.stubGlobal('navigator', { geolocation: { getCurrentPosition } });
    const pending = workflow('GPS_PENDING', ['GPS_SKIP', 'BACK'], { generation: 8, gps_status: 'pending' });
    const failed = workflow('GPS_CHOICE', ['REQUEST_GPS', 'GPS_SKIP', 'BACK'], { generation: 9, gps_status: 'failed' });
    const sendCommand = vi.fn(async (command: string, _generation: number, _location?: PhoneLocationInput) => ({
      ok: true as const,
      workflow: command === 'REQUEST_GPS' ? pending : failed,
    }));
    let renderer!: ReactTestRenderer;
    await act(async () => {
      renderer = create(createElement(WorkflowPanel, {
        status: statusFor('GPS_CHOICE', ACTIONS_BY_STEP.GPS_CHOICE),
        objectName: 'Quarry',
        sendCommand,
        onRefreshStatus: async () => true,
        onWorkflowUpdate: () => undefined,
        onOpenScanner: () => undefined,
      }));
    });

    const requestButton = renderer.root.findAllByType('button').find((button) =>
      button.children.includes(translate('en', 'workflow.action.request_gps')));
    await act(async () => {
      requestButton?.props.onClick();
      await Promise.resolve();
      await Promise.resolve();
      await Promise.resolve();
    });

    expect(getCurrentPosition).toHaveBeenCalledWith(expect.any(Function), expect.any(Function), expect.objectContaining({ maximumAge: 0 }));
    expect(sendCommand.mock.calls.map(([command, generation]) => [command, generation])).toEqual([
      ['REQUEST_GPS', 7],
      ['GPS_CAPTURE_FAILED', 8],
    ]);
    expect(renderer.root.findAllByProps({ role: 'status' }).some((node) =>
      node.children.includes(translate('en', 'workflow.gps.failed')))).toBe(true);
    act(() => renderer.unmount());
  });

  it('test_gps_timeout_failure_returns_to_choice_with_skip_and_back', async () => {
    vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
    const getCurrentPosition = vi.fn((_success: PositionCallback, failure: PositionErrorCallback) => {
      failure({ code: 3, message: 'position timeout', PERMISSION_DENIED: 1, POSITION_UNAVAILABLE: 2, TIMEOUT: 3 } as GeolocationPositionError);
    });
    vi.stubGlobal('navigator', { geolocation: { getCurrentPosition } });
    const pending = workflow('GPS_PENDING', ['GPS_SKIP', 'BACK'], { generation: 8, gps_status: 'pending' });
    const failed = workflow('GPS_CHOICE', ['REQUEST_GPS', 'GPS_SKIP', 'BACK'], { generation: 9, gps_status: 'failed' });
    const skipped = workflow('LAYOUT_READY', ['CALCULATE_POSITIONS', 'BACK'], { generation: 10, gps_status: 'skipped' });
    const sendCommand = vi.fn(async (command: string, _generation: number, _location?: PhoneLocationInput) => ({
      ok: true as const,
      workflow: command === 'REQUEST_GPS' ? pending : command === 'GPS_CAPTURE_FAILED' ? failed : skipped,
    }));
    const onWorkflowUpdate = vi.fn();
    const createPanel = (status: ReturnType<typeof statusFor>) => createElement(WorkflowPanel, {
      status,
      objectName: 'Quarry',
      sendCommand,
      onRefreshStatus: async () => true,
      onWorkflowUpdate,
      onOpenScanner: () => undefined,
    });
    let renderer!: ReactTestRenderer;
    await act(async () => {
      renderer = create(createPanel(statusFor('GPS_CHOICE', ACTIONS_BY_STEP.GPS_CHOICE)));
    });

    const findCommandButton = (command: string) => renderer.root.findAllByType('button').find((button) =>
      button.props['data-command'] === command);
    await act(async () => {
      findCommandButton('REQUEST_GPS')?.props.onClick();
      await Promise.resolve();
      await Promise.resolve();
      await Promise.resolve();
    });

    expect(getCurrentPosition).toHaveBeenCalledWith(expect.any(Function), expect.any(Function), expect.objectContaining({ maximumAge: 0 }));
    expect(sendCommand.mock.calls.map(([command, generation]) => [command, generation])).toEqual([
      ['REQUEST_GPS', 7],
      ['GPS_CAPTURE_FAILED', 8],
    ]);
    expect(onWorkflowUpdate).toHaveBeenCalledWith(failed);
    await act(async () => {
      renderer.update(createPanel(statusFor('GPS_CHOICE', ACTIONS_BY_STEP.GPS_CHOICE, null, {
        generation: 9,
        gps_status: 'failed',
      })));
    });
    expect(findCommandButton('REQUEST_GPS')).toBeDefined();
    expect(findCommandButton('GPS_SKIP')).toBeDefined();
    expect(findCommandButton('BACK')).toBeDefined();

    await act(async () => {
      findCommandButton('GPS_SKIP')?.props.onClick();
      await Promise.resolve();
      await Promise.resolve();
    });
    expect(sendCommand.mock.calls.at(-1)?.slice(0, 2)).toEqual(['GPS_SKIP', 9]);
    act(() => renderer.unmount());
  });

  it('captures GPS when another controller leaves the workflow at GPS_PENDING', async () => {
    vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
    const getCurrentPosition = vi.fn((success: PositionCallback) => {
      success({
        coords: {
          latitude: 49.0,
          longitude: 32.0,
          accuracy: 2.5,
          altitude: null,
          altitudeAccuracy: null,
          heading: null,
          speed: null,
        },
        timestamp: 1234.9,
      } as GeolocationPosition);
    });
    vi.stubGlobal('navigator', { geolocation: { getCurrentPosition } });
    const layout = workflow('LAYOUT_READY', ['CALCULATE_POSITIONS'], { generation: 52, gps_status: 'accepted' });
    const sendCommand = vi.fn(async () => ({ ok: true as const, workflow: layout }));
    let renderer!: ReactTestRenderer;
    await act(async () => {
      renderer = create(createElement(WorkflowPanel, {
        status: statusFor('GPS_PENDING', [...max460AllowedActions.GPS_PENDING], 4, {
          generation: 51,
          gps_status: 'pending',
        }),
        objectName: 'Quarry',
        sendCommand,
        onRefreshStatus: async () => true,
        onWorkflowUpdate: () => undefined,
        onOpenScanner: () => undefined,
      }));
      await Promise.resolve();
      await Promise.resolve();
      await Promise.resolve();
    });

    expect(getCurrentPosition).toHaveBeenCalledWith(expect.any(Function), expect.any(Function), expect.objectContaining({ maximumAge: 0 }));
    expect(sendCommand).toHaveBeenCalledWith('GPS_ACCEPT', 51, {
      latitude: 49.0,
      longitude: 32.0,
      accuracy_m: 2.5,
      altitude_m: null,
      altitude_accuracy_m: null,
      captured_at_ms: 1234,
    });
    act(() => renderer.unmount());
  });

  it('accepts a fresh fix only through GPS_ACCEPT and opens the phone scanner only after SCAN_START', async () => {
    vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
    const getCurrentPosition = vi.fn((success: PositionCallback) => {
      success({
        coords: {
          latitude: 49.0,
          longitude: 32.0,
          accuracy: 2.5,
          altitude: null,
          altitudeAccuracy: null,
          heading: null,
          speed: null,
        },
        timestamp: 1234.9,
      } as GeolocationPosition);
    });
    vi.stubGlobal('navigator', { geolocation: { getCurrentPosition } });
    const pending = workflow('GPS_PENDING', ['GPS_SKIP', 'BACK'], { generation: 8, gps_status: 'pending' });
    const layout = workflow('LAYOUT_READY', ['CALCULATE_POSITIONS'], { generation: 9, gps_status: 'accepted' });
    const sendCommand = vi.fn(async (command: string, _generation: number, _location?: PhoneLocationInput) => ({
      ok: true as const,
      workflow: command === 'REQUEST_GPS' ? pending : layout,
    }));
    const onOpenScanner = vi.fn();
    let renderer!: ReactTestRenderer;
    await act(async () => {
      renderer = create(createElement(WorkflowPanel, {
        status: statusFor('GPS_CHOICE', ACTIONS_BY_STEP.GPS_CHOICE),
        objectName: 'Quarry',
        sendCommand,
        onRefreshStatus: async () => true,
        onWorkflowUpdate: () => undefined,
        onOpenScanner,
      }));
    });
    const requestButton = renderer.root.findAllByType('button').find((button) =>
      button.children.includes(translate('en', 'workflow.action.request_gps')));
    await act(async () => {
      requestButton?.props.onClick();
      await Promise.resolve();
      await Promise.resolve();
      await Promise.resolve();
    });
    expect(sendCommand.mock.calls.map(([command, generation, location]) => [command, generation, location])).toEqual([
      ['REQUEST_GPS', 7, undefined],
      ['GPS_ACCEPT', 8, {
        latitude: 49.0,
        longitude: 32.0,
        accuracy_m: 2.5,
        altitude_m: null,
        altitude_accuracy_m: null,
        captured_at_ms: 1234,
      }],
    ]);

    const scanResult = workflow('SCANNING', ['BACK'], { object_id: 'OBJ_0001' });
    sendCommand.mockImplementation(async () => ({ ok: true, workflow: scanResult }));
    await act(async () => {
      renderer.update(createElement(WorkflowPanel, {
        status: statusFor('SCAN_CHOICE', ACTIONS_BY_STEP.SCAN_CHOICE),
        objectName: 'Quarry',
        sendCommand,
        onRefreshStatus: async () => true,
        onWorkflowUpdate: () => undefined,
        onOpenScanner,
      }));
    });
    const scanButton = renderer.root.findAllByType('button').find((button) =>
      button.children.includes(translate('en', 'workflow.action.scan_start')));
    await act(async () => {
      scanButton?.props.onClick();
      await Promise.resolve();
      await Promise.resolve();
    });
    expect(onOpenScanner).toHaveBeenCalledWith('OBJ_0001', null);
    act(() => renderer.unmount());
  });

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


it('keeps manual distance outcome unknown after a dropped reply and refreshes without retry', async () => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  const fetchMock = vi.fn(async () => { throw new Error('lost reply'); });
  vi.stubGlobal('fetch', fetchMock);
  const refresh = vi.fn(async () => true);
  const saved = vi.fn();
  let renderer!: ReactTestRenderer;
  await act(async () => {
    renderer = create(createElement(ManualDistanceForm, {
      pair: { node_a: 0, node_b: 1, distance_mm: 1000 } as RangingPairStatus,
      generation: 7, startedAtMs: 1, updatedAtMs: 2, onSaved: saved, onRefresh: refresh,
    }));
  });
  await act(async () => { renderer.root.findByType('button').props.onClick(); });
  await act(async () => { renderer.root.findByType('form').props.onSubmit({ preventDefault() {} }); });
  expect(renderer.root.findByProps({ role: 'alert' }).children.join(''))
    .toBe(translate('en', 'api.error_control_outcome_unknown'));
  expect(fetchMock).toHaveBeenCalledOnce();
  expect(saved).not.toHaveBeenCalled();
  expect(refresh).toHaveBeenCalledOnce();
  act(() => renderer.unmount());
});


it.each([
  ['cable_exceeded:0-1', 'The distance of pair 0-1 is longer than the cable allows; measure again or enter it manually.'],
  ['directions_disagree:1-4', 'Pair 1-4: the two directions measured different distances; measure again.'],
  ['triangle_inconsistent:1-2-4',
    'Distances between nodes 1-2-4 cannot form a triangle, so at least one of them is wrong; measure again.'],
])('explains the ranging failure %s in plain words instead of an unknown reason', (reason, english) => {
  const status = statusFor('RANGING_RESULT');
  (status as unknown as { ranging: unknown }).ranging = { available: true, progress: { failure_reason: reason } };
  const html = panelHtml(status);
  expect(html).toContain(english);
  expect(html).not.toContain(reason);
});

it('has the Ukrainian text of the new ranging failures', () => {
  expect(uk['workflow.ranging.reason.directions_disagree'])
    .toBe('Пара {pair}: два напрямки виміряли різні відстані; виміряйте ще раз.');
  expect(uk['workflow.ranging.reason.triangle_inconsistent']).toContain('трикутник');
  expect(uk['workflow.ranging.reason.triangle_inconsistent']).toContain('{nodes}');
  expect(uk['workflow.ranging.reason.cable_exceeded']).toContain('кабелю');
  expect(uk['workflow.ranging.reason.cable_exceeded']).toContain('{pair}');
});

it.each([
  ['pending', 'Wi-Fi turns off in a few seconds for the distance measurement.'],
  ['off', 'Wi-Fi is switched off while the distances are measured and comes back by itself.'],
  ['resuming', 'Wi-Fi is turning back on.'],
])('tells the user that the Wi-Fi radio is %s around the distance measurement', (radio, english) => {
  const status = statusFor('RANGING');
  (status as unknown as { ranging: unknown }).ranging = { available: false, radio };
  const html = panelHtml(status);
  expect(html).toContain(english);
  expect(html).toContain('workflow-ranging-wifi');
});

it('says nothing about the Wi-Fi radio when it is idle or when the firmware does not report it', () => {
  for (const ranging of [{ available: false, radio: 'idle' }, { available: false }]) {
    const status = statusFor('RANGING');
    (status as unknown as { ranging: unknown }).ranging = ranging;
    expect(panelHtml(status)).not.toContain('workflow-ranging-wifi');
  }
});

it('has the Ukrainian text of the Wi-Fi notices', () => {
  expect(uk['workflow.ranging.wifi_pending']).toContain('Wi-Fi вимкнеться');
  expect(uk['workflow.ranging.wifi_off']).toContain('Wi-Fi вимкнено');
  expect(uk['workflow.ranging.wifi_back']).toContain('вмикається знову');
});

it('explains the geometry consistency rejection instead of an unknown reason', () => {
  const status = statusFor('RANGING_RESULT');
  (status as unknown as { ranging: unknown }).ranging = { available: true, progress: { failure_reason: 'geometry_inconsistent' } };
  const html = panelHtml(status);
  expect(html).toContain('Measured distances disagree with the solved geometry.');
});
