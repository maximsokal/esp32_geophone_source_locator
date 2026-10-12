import { beforeAll, describe, expect, it, vi } from 'vitest';
import { createElement, type ReactNode } from 'react';
import { act, create, type ReactTestRenderer } from 'react-test-renderer';
import { renderToStaticMarkup } from 'react-dom/server';
import type { DeviceStatus, ObjectStoreState } from '../../src/domain/device';
import { normalizeDeviceStatus } from '../../src/api/deviceStatusSchema';
import ObjectsFeature from '../../src/features/objects/ObjectsFeature';
import SensorsFeature from '../../src/features/sensors/SensorsFeature';
import WorkflowPanel from '../../src/features/workflow/WorkflowPanel';
import { formatNumber } from '../../src/i18n/format';
import { fullDeviceStatus } from './fixtures/deviceStatusFull';
import { loadUkrainianCatalog, LocaleProvider, translate, type I18nLocale } from '../../src/i18n';

const livePanelTracker = vi.hoisted(() => ({ mounts: 0, renders: 0 }));

vi.mock('../../src/features/seismic/LiveSeismicPanel', async () => {
  const { createElement: element, useEffect } = await import('react');
  return {
    default: () => {
      livePanelTracker.renders += 1;
      useEffect(() => { livePanelTracker.mounts += 1; }, []);
      return element('div', { className: 'mock-live-seismic' }, 'LIVE_SEISMIC_PANEL');
    },
  };
});

function clone<T>(value: T): T {
  return JSON.parse(JSON.stringify(value)) as T;
}

function statusFor(step: string, actions: string[] = [], remotes = 4): DeviceStatus {
  const base = fullDeviceStatus();
  const pods = Array.from({ length: remotes }, (_, index) => ({
    ...base.pods[0],
    id: index + 1,
    state: 'READY',
  }));
  return {
    ...base,
    pods,
    recording: { ...base.recording, available: true, running: false },
    recording_gate: {
      available: true,
      assigned_count: remotes,
      accepted_count: remotes,
      decision: 'ALLOWED_3D_CAPABLE',
      dimensionality: 'CAPABLE_3D',
      allowed: true,
      bypassed: false,
      reason: '',
      warning: '',
    },
    acquisition: { ...base.acquisition, running: true },
    workflow: {
      ...base.workflow!,
      step,
      generation: 7,
      allowed_actions: actions,
      assigned_remotes: remotes,
      sensor_sufficiency: remotes >= 4 ? 'CAPABLE_3D' : remotes === 3 ? 'PLANAR_2D' : 'INSUFFICIENT',
    },
  } as DeviceStatus;
}

const store = {
  available: true,
  objects: [{
    id: 'OBJ_0001',
    name: 'Quarry',
    active: true,
    cable_lengths_m: [5, 5, 5, 5],
    location: {
      valid: false, latitude: 0, longitude: 0, accuracy_m: 0,
      captured_at_ms: 0, source: '', altitude_valid: false, altitude_m: 0, altitude_accuracy_m: 0,
    },
  }],
  default_cable_lengths_m: [5, 5, 5, 5],
} as unknown as ObjectStoreState;

function render(locale: I18nLocale, child: ReactNode): string {
  return renderToStaticMarkup(createElement(LocaleProvider, { initialLocale: locale }, child));
}

function objectProps(status: DeviceStatus) {
  return {
    status, store, sessions: null, selectedId: 'OBJ_0001', busy: false, error: null,
    onSelect: () => undefined, onCreate: async () => undefined, onActivate: async () => undefined,
    onRename: async () => undefined, onDelete: async () => undefined,
    onBindGps: async () => ({ latitude: 0, longitude: 0, accuracy_m: 1, captured_at_ms: 1, source: 'test' }),
    onCable: async () => undefined, onDefaultCable: async () => undefined,
    onAcquisition: async () => undefined, onRecording: async () => undefined,
    onStartWorkflow: () => undefined, onOpenScanner: () => undefined,
  };
}

function objectsHtml(status: DeviceStatus, locale: I18nLocale = 'en'): string {
  return render(locale, createElement(ObjectsFeature, objectProps(status)));
}

function workflowHtml(status: DeviceStatus, locale: I18nLocale): string {
  return render(locale, createElement(WorkflowPanel, {
    status, objectName: 'Quarry',
    sendCommand: vi.fn().mockResolvedValue({ ok: true, workflow: status.workflow }),
    onRefreshStatus: async () => true, onWorkflowUpdate: () => undefined, onOpenScanner: () => undefined,
  }));
}

function sensorsHtml(status: DeviceStatus, locale: I18nLocale): string {
  return render(locale, createElement(SensorsFeature, {
    status, object: null, error: null, onCalibrateHeights: async () => true,
  }));
}

beforeAll(async () => { await loadUkrainianCatalog(); });

describe('MAX-452 recording and acquisition controls', () => {
  it.each(['OBJECT', 'SENSOR_CHECK', 'CALIB_PREP', 'RECORD_READY', 'RECORDING'] as const)(
    'offers direct Record only in allowed workflow steps (%s)',
    (step) => {
      const html = objectsHtml(statusFor(step));
      if (step === 'RECORD_READY' || step === 'RECORDING') {
        expect(html).toContain(translate('en', 'objects.record_session'));
      } else {
        expect(html).not.toContain(translate('en', 'objects.record_session'));
        expect(html).toContain('Workflow tab');
      }
    },
  );

  it('hides acquisition/live controls outside RECORD_READY and RECORDING', () => {
    const hidden = objectsHtml(statusFor('SENSOR_CHECK'));
    const visible = objectsHtml(statusFor('RECORD_READY'));
    expect(hidden).not.toContain(translate('en', 'objects.start_acquisition'));
    expect(hidden).not.toContain(translate('en', 'objects.stop_acquisition'));
    expect(hidden).not.toContain(translate('en', 'objects.live_signal'));
    expect(visible).toContain(translate('en', 'objects.stop_acquisition'));
    expect(visible).toContain(translate('en', 'objects.live_signal'));
  });

  it('mounts the live panel only after an allowed-step button opens it', async () => {
    vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
    livePanelTracker.mounts = 0;
    livePanelTracker.renders = 0;
    let blocked!: ReactTestRenderer;
    let allowed!: ReactTestRenderer;
    await act(async () => {
      blocked = create(createElement(ObjectsFeature, objectProps(statusFor('SENSOR_CHECK'))));
      allowed = create(createElement(ObjectsFeature, objectProps(statusFor('RECORD_READY'))));
    });
    expect(blocked.root.findAll((node) => node.props.className === 'mock-live-seismic')).toHaveLength(0);
    const button = allowed.root.findAllByType('button').find((item) =>
      item.children.join('').includes(translate('en', 'objects.live_signal')));
    expect(button).toBeDefined();
    await act(async () => {
      button?.props.onClick();
      await Promise.resolve();
      await Promise.resolve();
    });
    expect(allowed.root.findAll((node) => node.props.className === 'mock-live-seismic')).toHaveLength(1);
    const mountsAfterOpen = livePanelTracker.mounts;
    const rendersAfterOpen = livePanelTracker.renders;
    await act(async () => {
      allowed.update(createElement(ObjectsFeature, objectProps(statusFor('SENSOR_CHECK'))));
    });
    expect(livePanelTracker.mounts).toBe(mountsAfterOpen);
    expect(livePanelTracker.renders).toBe(rendersAfterOpen);
    expect(allowed.root.findAll((node) => node.props.className === 'mock-live-seismic')).toHaveLength(0);
    act(() => { blocked.unmount(); allowed.unmount(); });
  });
});

describe('MAX-452 recording bypass and sensor status display', () => {
  it.each(['en', 'uk'] as const)('explains recording bypass with reason and counts in %s', (locale) => {
    const status = statusFor('RECORD_READY');
    status.recording_gate = {
      available: true, assigned_count: 2, accepted_count: 1,
      decision: 'BLOCKED_INSUFFICIENT_REMOTES', dimensionality: 'INSUFFICIENT',
      allowed: true, bypassed: true, reason: 'fewer than 3 REMOTE geophones are active/assigned',
    };
    expect(objectsHtml(status, locale)).toContain(translate(locale, 'recording_gate.bypass_allowed', {
      reason: translate(locale, 'recording_gate.reason.insufficient_remotes'), assigned: 2, accepted: 1,
    }));
  });

  it('shows no bypass indicator when bypassed is false', () => {
    const status = statusFor('RECORD_READY', [], 2);
    status.recording_gate = {
      available: true, assigned_count: 2, accepted_count: 1,
      decision: 'BLOCKED_INSUFFICIENT_REMOTES', dimensionality: 'INSUFFICIENT',
      allowed: true, bypassed: false, reason: 'BLOCKED_INSUFFICIENT_REMOTES',
    };
    expect(objectsHtml(status)).not.toContain(translate('en', 'recording_gate.bypass_allowed', {
      reason: translate('en', 'recording_gate.reason.insufficient_remotes'), assigned: 2, accepted: 1,
    }));
  });

  it.each(['en', 'uk'] as const)('renders four localized link states, colors and CH-to-node mapping in %s', (locale) => {
    const status = statusFor('RECORD_READY');
    (status as unknown as { node_links: unknown[] }).node_links = [
      { channel: 1, node: 'MAIN', status: 'NOT_CONNECTED' },
      { channel: 2, node: 'G1', status: 'CONNECTED' },
      { channel: 3, node: 'G2', status: 'READY' },
      { channel: 4, node: 'G3', status: 'MEASURING' },
    ];
    const html = sensorsHtml(status, locale);
    const rows = [
      ['CH1', 'MAIN', 'not-connected', 'sensors.node_link.not_connected'],
      ['CH2', 'G1', 'connected', 'sensors.node_link.connected'],
      ['CH3', 'G2', 'ready', 'sensors.node_link.ready'],
      ['CH4', 'G3', 'measuring', 'sensors.node_link.measuring'],
    ] as const;
    for (const [channel, node, color, key] of rows) {
      expect(html).toContain(channel);
      expect(html).toContain(node);
      expect(html).toContain(translate(locale, key));
      expect(html).toContain(
        `<div class="node-link-row"><strong>${channel} → ${node}</strong><span class="node-link-status node-link-status--${color}">`,
      );
    }
  });

  it.each(['en', 'uk'] as const)('marks stale power and topology snapshots in %s', (locale) => {
    const status = statusFor('RECORD_READY');
    const channels = status.power.channels ?? [];
    channels[1] = { ...channels[1], age_ms: 4999, stale: true };
    (status as unknown as { topology: unknown }).topology = { valid: true, age_ms: 25000, stale: true };
    const html = sensorsHtml(status, locale);
    expect(html).toContain(translate(locale, 'sensors.power_snapshot_stale', { channel: 'ESP32_5V', age: formatNumber(locale, 4999) }));
    expect(html).toContain(translate(locale, 'sensors.topology_snapshot_stale', { age: formatNumber(locale, 25000) }));
  });

  it('localizes known pod states in both languages and labels unknown state safely', () => {
    const status = statusFor('RECORD_READY');
    status.pods = status.pods.slice(0, 4).map((pod, index) => ({
        ...pod, state: ['DISCONNECTED', 'READY', 'WARNING', 'ERROR'][index],
      }));
    const english = sensorsHtml(status, 'en');
    const ukrainian = sensorsHtml(status, 'uk');
    for (const [code, key] of [
      ['DISCONNECTED', 'sensors.pod_state.disconnected'],
      ['READY', 'sensors.pod_state.ready'],
      ['WARNING', 'sensors.pod_state.warning'],
      ['ERROR', 'sensors.pod_state.error'],
    ] as const) {
      expect(english).toContain(translate('en', key));
      expect(ukrainian).toContain(translate('uk', key));
    }
    const unknownStatus = statusFor('RECORD_READY');
    unknownStatus.pods[0].state = 'BOOTING';
    expect(sensorsHtml(unknownStatus, 'en')).toContain(translate('en', 'sensors.pod_state.unknown', { state: 'BOOTING' }));
    expect(sensorsHtml(unknownStatus, 'uk')).toContain(translate('uk', 'sensors.pod_state.unknown', { state: 'BOOTING' }));
  });
});

describe('MAX-452 sensor guidance and freshness schema', () => {
  it.each(['en', 'uk'] as const)('keeps GO_CALIBRATION available with limited-solution guidance in %s', (locale) => {
    for (const remotes of [0, 1, 2]) {
      const html = workflowHtml(statusFor('SENSOR_CHECK', ['GO_CALIBRATION'], remotes), locale);
      expect(html).toContain(translate(locale, 'workflow.sensor_check.limited_calibration'));
      expect(html).toContain(translate(locale, 'workflow.action.go_calibration'));

      const withoutCalibration = workflowHtml(statusFor('SENSOR_CHECK', [], remotes), locale);
      expect(withoutCalibration).toContain(translate(locale, 'workflow.sensor_check.insufficient'));
      expect(withoutCalibration).not.toContain(translate(locale, 'workflow.sensor_check.limited_calibration'));
    }
  });

  it.each([
    [0, false, 0, false],
    [4999, true, 24999, false],
    [5000, true, 25000, true],
  ] as const)('validates freshness at power/topology boundaries (%s ms)', (powerAge, powerStale, topologyAge, topologyStale) => {
    const raw = clone(fullDeviceStatus()) as unknown as Record<string, unknown>;
    const power = raw.power as { channels: Array<Record<string, unknown>> };
    power.channels[0].age_ms = powerAge;
    power.channels[0].stale = powerStale;
    raw.topology = { valid: true, age_ms: topologyAge, stale: topologyStale };
    const normalized = normalizeDeviceStatus(raw);
    expect(normalized.power.channels?.[0]).toMatchObject({ age_ms: powerAge, stale: powerStale });
    expect((normalized as unknown as { topology: { age_ms: number; stale: boolean } }).topology)
      .toMatchObject({ age_ms: topologyAge, stale: topologyStale });
  });

  it('rejects malformed freshness fields', () => {
    const raw = clone(fullDeviceStatus()) as unknown as Record<string, unknown>;
    const power = raw.power as { channels: Array<Record<string, unknown>> };
    power.channels[0].age_ms = 'old';
    expect(() => normalizeDeviceStatus(raw)).toThrow();
  });
});

describe('MAX-452 workflow reason localization', () => {
  it.each(['en', 'uk'] as const)('translates known calibration/ranging codes and safely falls back in %s', (locale) => {
    const status = statusFor('CALIB_RESULT');
    status.workflow!.calibration.reasons = [
      'insufficient_nodes', 'cs_spread_too_high', 'cs_pair_invalid:1-2',
      'G1:height_rejected_moving', 'timeout', 'future_reason',
    ];
    const html = workflowHtml(status, locale);
    for (const key of [
      'workflow.calibration.reason.insufficient_nodes',
      'workflow.calibration.reason.cs_spread_too_high',
      'workflow.calibration.reason.cs_pair_invalid',
      'workflow.calibration.reason.height_rejected_moving',
      'workflow.calibration.reason.timeout',
    ] as const) {
      expect(html).toContain(translate(locale, key, { pair: '1-2', node: 'G1' }));
    }
    expect(html).toContain(translate(locale, 'workflow.reason.unknown', { reason: 'future_reason' }));

    const ranging = statusFor('RANGING');
    if (ranging.ranging?.progress) ranging.ranging.progress.failure_reason = 'pair_failed:1-2';
    expect(workflowHtml(ranging, locale)).toContain(
      translate(locale, 'workflow.ranging.reason.pair_failed', { pair: '1-2' }),
    );
  });
});
