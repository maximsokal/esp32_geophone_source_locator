import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createElement, useState } from 'react';
import { act, create, type ReactTestRenderer } from 'react-test-renderer';
import {
  loadUkrainianCatalog,
  translate,
} from '../../src/i18n';
import type {
  CalcSettingsProfile,
  CalcSettingsResponse,
} from '../../src/api/deviceApi';
import { ApiRequestOwnerRetiredError, ApiRequestPreemptedError } from '../../src/api/deviceApi';
import type { DeviceStatus } from '../../src/domain/device';
import MaterialSettingsForm from '../../src/features/objects/MaterialSettingsForm';
import LocalizationCalculatePanel from '../../src/features/objects/LocalizationCalculatePanel';
import WorkflowPanel from '../../src/features/workflow/WorkflowPanel';
import WorkflowResultViewer from '../../src/features/workflow/WorkflowResultViewer';

const api = vi.hoisted(() => ({
  getCalcSettings: vi.fn(),
  saveCalcSettings: vi.fn(),
  fetchLocalizationRuns: vi.fn(),
  fetchScanInfo: vi.fn(),
  fetchDeviceStatus: vi.fn(),
  sendControl: vi.fn(),
}));

vi.mock('../../src/api/deviceApi', async (importOriginal) => ({
  ...await importOriginal<typeof import('../../src/api/deviceApi')>(),
  ...api,
}));

const STANDARD = {
  timing_uncertainty_us: 2,
  noise_window_samples: 100,
  sta_window_samples: 20,
  lta_window_samples: 200,
  sta_lta_trigger_ratio: 3,
  refine_lookback_samples: 20,
  refine_lookahead_samples: 20,
  refine_amplitude_fraction: 0.5,
  snr_signal_window_samples: 40,
  minimum_snr: 2,
  semblance_window_half_span_samples: 8,
  s_pick_enabled: false,
  time_budget_s: 60,
};

function profile(overrides: Partial<CalcSettingsProfile> = {}): CalcSettingsProfile {
  return {
    material: 'concrete',
    propagation_speed_mps: 4000,
    propagation_speed_uncertainty_mps: 500,
    ...STANDARD,
    ...overrides,
  };
}

function settingsResponse(settings: CalcSettingsProfile | null = null): CalcSettingsResponse {
  return {
    settings,
    presets: [
      { id: 'concrete', propagation_speed_mps: 4000, propagation_speed_uncertainty_mps: 500 },
      { id: 'reinforced_concrete', propagation_speed_mps: 4200, propagation_speed_uncertainty_mps: 500 },
      { id: 'brick_masonry', propagation_speed_mps: 2200, propagation_speed_uncertainty_mps: 700 },
      { id: 'rubble', propagation_speed_mps: 400, propagation_speed_uncertainty_mps: 250 },
      { id: 'steel', propagation_speed_mps: 5200, propagation_speed_uncertainty_mps: 700 },
      { id: 'soil_dry', propagation_speed_mps: 500, propagation_speed_uncertainty_mps: 300 },
      { id: 'soil_saturated', propagation_speed_mps: 1500, propagation_speed_uncertainty_mps: 500 },
    ],
    standardProcessing: STANDARD,
  };
}

function runListing(runs: unknown[] = []): unknown {
  return {
    geometry: { available: false, pods: [] },
    cs_pairs: { available: false, pairs: [] },
    picks: [],
    runs,
  };
}

async function mount(element: React.ReactElement): Promise<ReactTestRenderer> {
  let renderer!: ReactTestRenderer;
  await act(async () => {
    renderer = create(element);
    await Promise.resolve();
    await Promise.resolve();
  });
  return renderer;
}

function input(renderer: ReactTestRenderer, label: string) {
  return renderer.root.findAllByType('input').find((node) => node.props['aria-label'] === label)!;
}

function button(renderer: ReactTestRenderer, key: string) {
  return renderer.root.findAllByType('button')
    .find((node) => node.children.join('') === translate('en', key))!;
}

beforeEach(() => {
  api.getCalcSettings.mockReset().mockResolvedValue(settingsResponse());
  api.saveCalcSettings.mockReset().mockResolvedValue({ ok: true });
  api.fetchLocalizationRuns.mockReset().mockResolvedValue(runListing());
  api.fetchScanInfo.mockReset().mockResolvedValue({ present: false, bytes: 0 });
  api.fetchDeviceStatus.mockReset().mockResolvedValue({ recording_sessions: { sessions: [] } });
  api.sendControl.mockReset().mockResolvedValue({ ok: false, action: 'calculate_source', error: 'test error' });
});

afterEach(() => {
  vi.restoreAllMocks();
});

describe('material settings form', () => {
  it('retries a preempted initial settings read without showing a load error', async () => {
    api.getCalcSettings.mockRejectedValueOnce(new ApiRequestPreemptedError())
      .mockResolvedValueOnce(settingsResponse());
    const renderer = await mount(createElement(MaterialSettingsForm, { objectId: 'OBJ_0001' }));
    expect(api.getCalcSettings).toHaveBeenCalledTimes(2);
    expect(renderer.root.findAllByProps({ role: 'alert' })).toHaveLength(0);
    expect(renderer.root.findByType('select')).toBeTruthy();
    renderer.unmount();
  });

  it('silently drops a retired settings read', async () => {
    api.getCalcSettings.mockRejectedValueOnce(new ApiRequestOwnerRetiredError());
    const renderer = await mount(createElement(MaterialSettingsForm, { objectId: 'OBJ_0001' }));
    expect(api.getCalcSettings).toHaveBeenCalledTimes(1);
    expect(renderer.root.findAllByProps({ role: 'alert' })).toHaveLength(0);
    renderer.unmount();
  });

  it('loads preset values from the API and changes an edited preset to Custom', async () => {
    const renderer = await mount(createElement(MaterialSettingsForm, { objectId: 'OBJ_0001' }));
    const select = renderer.root.findByType('select');
    expect(input(renderer, translate('en', 'calc.propagation_speed')).props.disabled).toBe(true);
    await act(async () => { select.props.onChange({ target: { value: 'concrete' } }); });
    expect(input(renderer, translate('en', 'calc.propagation_speed')).props.disabled).toBe(false);
    expect(input(renderer, translate('en', 'calc.propagation_speed')).props.value).toBe('4000');
    expect(input(renderer, translate('en', 'calc.speed_uncertainty')).props.value).toBe('500');

    await act(async () => {
      input(renderer, translate('en', 'calc.propagation_speed')).props.onChange({ target: { value: '4100' } });
    });
    expect(select.props.value).toBe('custom');
    expect(JSON.stringify(renderer.toJSON())).toContain(translate('en', 'workflow.material.switched_to_custom'));
    renderer.unmount();
  });

  it('shows localized speed and uncertainty validation in both catalogs', async () => {
    await loadUkrainianCatalog();
    const renderer = await mount(createElement(MaterialSettingsForm, { objectId: 'OBJ_0001' }));
    const select = renderer.root.findByType('select');
    await act(async () => { select.props.onChange({ target: { value: 'custom' } }); });
    await act(async () => {
      input(renderer, translate('en', 'calc.propagation_speed')).props.onChange({ target: { value: '50' } });
      input(renderer, translate('en', 'calc.speed_uncertainty')).props.onChange({ target: { value: '50' } });
    });
    const html = JSON.stringify(renderer.toJSON());
    expect(html).toContain(translate('en', 'workflow.material.validation.speed'));
    expect(html).toContain(translate('en', 'workflow.material.validation.uncertainty'));
    await act(async () => {
      input(renderer, translate('en', 'calc.propagation_speed')).props.onChange({ target: { value: '400' } });
      input(renderer, translate('en', 'calc.speed_uncertainty')).props.onChange({ target: { value: '200' } });
      input(renderer, translate('en', 'calc.sta_window')).props.onChange({ target: { value: '200' } });
      input(renderer, translate('en', 'calc.lta_window')).props.onChange({ target: { value: '100' } });
    });
    expect(JSON.stringify(renderer.toJSON())).toContain(translate('en', 'workflow.material.validation.sta_lta'));
    await act(async () => {
      input(renderer, translate('en', 'calc.sta_window')).props.onChange({ target: { value: '100' } });
      input(renderer, translate('en', 'calc.lta_window')).props.onChange({ target: { value: '200' } });
    });
    expect(JSON.stringify(renderer.toJSON())).toContain(translate('en', 'workflow.material.validation.ratio'));
    for (const key of [
      'workflow.material.validation.speed',
      'workflow.material.validation.uncertainty',
      'workflow.material.validation.sta_lta',
      'workflow.material.validation.ratio',
    ] as const) {
      expect(translate('uk', key)).not.toBe(translate('en', key));
    }
    renderer.unmount();
  });

  it('sends only changed advanced fields as overrides', async () => {
    const renderer = await mount(createElement(MaterialSettingsForm, { objectId: 'OBJ_0001' }));
    await act(async () => {
      renderer.root.findByType('select').props.onChange({ target: { value: 'concrete' } });
      input(renderer, translate('en', 'calc.sta_window')).props.onChange({ target: { value: '40' } });
    });
    await act(async () => {
      button(renderer, 'workflow.material.save').props.onClick();
      await Promise.resolve();
      await Promise.resolve();
      await Promise.resolve();
    });
    expect(api.saveCalcSettings).toHaveBeenCalledWith('OBJ_0001', {
      material: 'concrete',
      sta_window_samples: 40,
    });
    renderer.unmount();
  });

  it('preserves saved nonstandard processing settings as API overrides', async () => {
    api.getCalcSettings.mockResolvedValueOnce(settingsResponse(profile({
      lta_window_samples: 250,
      time_budget_s: 120,
      s_pick_enabled: true,
      s_min_separation_samples: 120,
      s_max_separation_samples: 800,
      s_sta_window_samples: 30,
      s_lta_window_samples: 300,
      s_sta_lta_trigger_ratio: 4,
      s_minimum_snr: 3,
    })));
    const renderer = await mount(createElement(MaterialSettingsForm, { objectId: 'OBJ_0001' }));
    await act(async () => {
      button(renderer, 'workflow.material.save').props.onClick();
      await Promise.resolve();
      await Promise.resolve();
      await Promise.resolve();
    });
    expect(api.saveCalcSettings).toHaveBeenCalledWith('OBJ_0001', {
      material: 'concrete',
      lta_window_samples: 250,
      time_budget_s: 120,
      s_pick_enabled: true,
      s_min_separation_samples: 120,
      s_max_separation_samples: 800,
      s_sta_window_samples: 30,
      s_lta_window_samples: 300,
      s_sta_lta_trigger_ratio: 4,
      s_minimum_snr: 3,
    });
    renderer.unmount();
  });

  it('shows a localized save error returned by the API', async () => {
    api.saveCalcSettings.mockResolvedValue({ ok: false, error: 'invalid material', httpStatus: 400 });
    const renderer = await mount(createElement(MaterialSettingsForm, { objectId: 'OBJ_0001' }));
    await act(async () => {
      renderer.root.findByType('select').props.onChange({ target: { value: 'concrete' } });
    });
    await act(async () => {
      button(renderer, 'workflow.material.save').props.onClick();
      await Promise.resolve();
      await Promise.resolve();
    });
    expect(JSON.stringify(renderer.toJSON())).toContain('invalid material');
    expect(JSON.stringify(renderer.toJSON())).toContain('HTTP 400');
    renderer.unmount();
  });
});

it('prefills the manual calculate form from the saved object settings', async () => {
  api.getCalcSettings.mockResolvedValue(settingsResponse(profile({
    material: 'custom',
    propagation_speed_mps: 4550,
    propagation_speed_uncertainty_mps: 350,
    sta_window_samples: 25,
    lta_window_samples: 250,
  })));
  const renderer = await mount(createElement(LocalizationCalculatePanel, {
    objectId: 'OBJ_0001',
    sessionId: 'S000001',
    busy: false,
    onClose: () => undefined,
  }));
  expect(renderer.root.findByProps({ 'data-testid': 'manual-propagation-speed' }).props.value).toBe('4550');
  expect(renderer.root.findByProps({ 'data-testid': 'manual-sta-window' }).props.value).toBe('25');
  expect(renderer.root.findByProps({ 'data-testid': 'manual-lta-window' }).props.value).toBe('250');
  renderer.unmount();
});

it('retries a preempted localization listing without showing a geometry error', async () => {
  api.fetchLocalizationRuns.mockRejectedValueOnce(new ApiRequestPreemptedError())
    .mockResolvedValueOnce(runListing());
  const renderer = await mount(createElement(LocalizationCalculatePanel, {
    objectId: 'OBJ_0001',
    sessionId: 'S000001',
    busy: false,
    onClose: () => undefined,
  }));
  expect(api.fetchLocalizationRuns).toHaveBeenCalledTimes(2);
  expect(renderer.root.findAllByProps({ role: 'alert' })).toHaveLength(0);
  renderer.unmount();
});

it('silently drops a retired localization listing without leaving the geometry spinner active', async () => {
  api.fetchLocalizationRuns.mockRejectedValueOnce(new ApiRequestOwnerRetiredError());
  const renderer = await mount(createElement(LocalizationCalculatePanel, {
    objectId: 'OBJ_0001',
    sessionId: 'S000001',
    busy: false,
    onClose: () => undefined,
  }));
  expect(api.fetchLocalizationRuns).toHaveBeenCalledTimes(1);
  expect(renderer.root.findAllByProps({ role: 'alert' })).toHaveLength(0);
  expect(JSON.stringify(renderer.toJSON())).not.toContain(translate('en', 'calc.loading_geometry'));
  renderer.unmount();
});

async function calculatePanel(settings: CalcSettingsProfile): Promise<ReactTestRenderer> {
  api.getCalcSettings.mockResolvedValueOnce(settingsResponse(settings));
  return mount(createElement(LocalizationCalculatePanel, {
    objectId: 'OBJ_0001',
    sessionId: 'S000001',
    busy: false,
    onClose: () => undefined,
  }));
}

async function submitCalculate(renderer: ReactTestRenderer): Promise<Record<string, unknown>> {
  await act(async () => {
    button(renderer, 'calc.calculate').props.onClick();
    await Promise.resolve();
  });
  expect(api.sendControl).toHaveBeenCalledWith('calculate_source', expect.any(Object));
  return api.sendControl.mock.calls[0][1] as Record<string, unknown>;
}

it('sends the saved preset id when the manual values are unchanged', async () => {
  const renderer = await calculatePanel(profile());
  expect(await submitCalculate(renderer)).toMatchObject({ material: 'concrete' });
  renderer.unmount();
});

it('sends custom after a speed edit and restores the preset id when edited back', async () => {
  const renderer = await calculatePanel(profile());
  const speed = renderer.root.findByProps({ 'data-testid': 'manual-propagation-speed' });
  await act(async () => speed.props.onChange({ target: { value: '4100' } }));
  expect(await submitCalculate(renderer)).toMatchObject({ material: 'custom' });

  api.sendControl.mockClear();
  await act(async () => speed.props.onChange({ target: { value: '4000' } }));
  expect(await submitCalculate(renderer)).toMatchObject({ material: 'concrete' });
  renderer.unmount();
});

it('keeps a custom-material object custom in the calculate request', async () => {
  const renderer = await calculatePanel(profile({
    material: 'custom',
    propagation_speed_mps: 4550,
    propagation_speed_uncertainty_mps: 350,
  }));
  expect(await submitCalculate(renderer)).toMatchObject({ material: 'custom' });
  renderer.unmount();
});

it('refreshes workflow status after saving so CALCULATE_SOURCES appears', async () => {
  const stopped = (ready: boolean): DeviceStatus => ({
    workflow: {
      step: 'STOPPED',
      generation: 7,
      allowed_actions: ready ? ['CALCULATE_SOURCES'] : [],
      calibration: {},
      placement: {},
      object_id: 'OBJ_0001',
      session_id: 'S000001',
      calc_settings_ready: ready,
    },
  } as unknown as DeviceStatus);
  const Harness = () => {
    const [status, setStatus] = useState(stopped(false));
    return createElement(WorkflowPanel, {
      status,
      objectName: 'Quarry',
      sendCommand: async () => ({ ok: false, reason: 'not_allowed' }),
      onRefreshStatus: async () => { setStatus(stopped(true)); return true; },
      onWorkflowUpdate: () => undefined,
      onOpenScanner: () => undefined,
    });
  };
  api.getCalcSettings.mockResolvedValueOnce(settingsResponse(null)).mockResolvedValueOnce(settingsResponse(profile()));
  const renderer = await mount(createElement(Harness));
  await act(async () => {
    renderer.root.findByType('select').props.onChange({ target: { value: 'concrete' } });
  });
  await act(async () => {
    button(renderer, 'workflow.material.save').props.onClick();
    await Promise.resolve();
    await Promise.resolve();
    await Promise.resolve();
    await Promise.resolve();
  });
  expect(JSON.stringify(renderer.toJSON())).toContain(translate('en', 'workflow.action.calculate_sources'));
  expect(JSON.stringify(renderer.toJSON())).toContain(translate('en', 'workflow.material.current', {
    name: translate('en', 'workflow.material.name.concrete'),
    speed: '4,000',
    sigma: '500',
  }));
  renderer.unmount();
});

it('shows a run material name in the workflow result view', async () => {
  api.fetchLocalizationRuns.mockResolvedValue(runListing([{
    material: 'brick_masonry', computed_at_ms: 1, solved: true,
    source_position: { x_m: 1, y_m: 2, z_m: 3 },
  }]));
  const renderer = await mount(createElement(WorkflowResultViewer, {
    objectId: 'OBJ_0001',
    sessionId: 'S000001',
  }));
  expect(JSON.stringify(renderer.toJSON())).toContain(translate('en', 'workflow.material.run', {
    name: translate('en', 'workflow.material.name.brick_masonry'),
  }));
  renderer.unmount();
});

it('retries a preempted workflow result read without showing a failure alert', async () => {
  api.fetchLocalizationRuns.mockRejectedValueOnce(new ApiRequestPreemptedError())
    .mockResolvedValueOnce(runListing());
  const renderer = await mount(createElement(WorkflowResultViewer, {
    objectId: 'OBJ_0001',
    sessionId: 'S000001',
  }));
  expect(api.fetchLocalizationRuns).toHaveBeenCalledTimes(2);
  expect(renderer.root.findAllByProps({ role: 'alert' })).toHaveLength(0);
  renderer.unmount();
});

it('silently drops a retired workflow result read without showing a failure alert', async () => {
  api.fetchLocalizationRuns.mockRejectedValueOnce(new ApiRequestOwnerRetiredError());
  const renderer = await mount(createElement(WorkflowResultViewer, {
    objectId: 'OBJ_0001',
    sessionId: 'S000001',
  }));
  expect(api.fetchLocalizationRuns).toHaveBeenCalledTimes(1);
  expect(renderer.root.findAllByProps({ role: 'alert' })).toHaveLength(0);
  renderer.unmount();
});

/* MAX-301 S5: an explicit velocity range. */
describe('velocity range editor', () => {
  it('saves a custom min/max as a user velocity range', async () => {
    const renderer = await mount(createElement(MaterialSettingsForm, { objectId: 'OBJ_0001' }));
    await act(async () => { renderer.root.findByType('select').props.onChange({ target: { value: 'custom' } }); });
    await act(async () => {
      input(renderer, translate('en', 'calc.propagation_speed')).props.onChange({ target: { value: '400' } });
      input(renderer, translate('en', 'calc.speed_uncertainty')).props.onChange({ target: { value: '200' } });
      input(renderer, translate('en', 'workflow.material.range_min')).props.onChange({ target: { value: '150' } });
      input(renderer, translate('en', 'workflow.material.range_max')).props.onChange({ target: { value: '1200' } });
    });
    await act(async () => {
      button(renderer, 'workflow.material.save').props.onClick();
      await Promise.resolve();
      await Promise.resolve();
      await Promise.resolve();
    });
    expect(api.saveCalcSettings).toHaveBeenCalledWith('OBJ_0001', expect.objectContaining({
      material: 'custom',
      velocity_range: { v_min_mps: 150, v_max_mps: 1200, vp_vs_min: 0, vp_vs_max: 0, source: 'user' },
    }));
    renderer.unmount();
  });

  it('blocks saving a range wider than 10x or with one limit missing, in both languages', async () => {
    await loadUkrainianCatalog();
    const renderer = await mount(createElement(MaterialSettingsForm, { objectId: 'OBJ_0001' }));
    await act(async () => { renderer.root.findByType('select').props.onChange({ target: { value: 'custom' } }); });
    await act(async () => {
      input(renderer, translate('en', 'calc.propagation_speed')).props.onChange({ target: { value: '400' } });
      input(renderer, translate('en', 'calc.speed_uncertainty')).props.onChange({ target: { value: '200' } });
      input(renderer, translate('en', 'workflow.material.range_min')).props.onChange({ target: { value: '100' } });
      input(renderer, translate('en', 'workflow.material.range_max')).props.onChange({ target: { value: '1500' } });
    });
    expect(JSON.stringify(renderer.toJSON())).toContain(translate('en', 'workflow.material.validation.range'));
    await act(async () => {
      input(renderer, translate('en', 'workflow.material.range_max')).props.onChange({ target: { value: '' } });
    });
    expect(JSON.stringify(renderer.toJSON())).toContain(translate('en', 'workflow.material.validation.range'));
    expect(translate('uk', 'workflow.material.validation.range')).not.toBe(translate('en', 'workflow.material.validation.range'));
    renderer.unmount();
  });

  it('sends a stored user range with the calculate request while speed and sigma are unchanged', async () => {
    const userRange = { v_min_mps: 150, v_max_mps: 1200, vp_vs_min: 0, vp_vs_max: 0, source: 'user' };
    const renderer = await calculatePanel(profile({
      material: 'custom',
      propagation_speed_mps: 400,
      propagation_speed_uncertainty_mps: 200,
      velocity_range: userRange,
    } as unknown as Partial<CalcSettingsProfile>));
    expect(await submitCalculate(renderer)).toMatchObject({ material: 'custom', velocity_range: userRange });
    renderer.unmount();
  });

  it('lets the device derive the range for a preset object', async () => {
    const renderer = await calculatePanel(profile());
    expect(await submitCalculate(renderer)).not.toHaveProperty('velocity_range');
    renderer.unmount();
  });
});
