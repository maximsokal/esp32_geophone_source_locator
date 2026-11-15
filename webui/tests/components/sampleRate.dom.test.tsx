import type { ComponentProps } from 'react';
import { screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';
import ObjectsFeature from '../../src/features/objects/ObjectsFeature';
import SampleRateSelect from '../../src/features/objects/SampleRateSelect';
import WorkflowPanel from '../../src/features/workflow/WorkflowPanel';
import { translate } from '../../src/i18n';
import type { DeviceAcquisitionStatus } from '../../src/domain/device';
import { readyStatus, renderUi, respondJson } from './setup';

const OPTIONS = [
  { sample_rate_hz: 500, max_record_s: 0 },
  { sample_rate_hz: 2000, max_record_s: 120 },
  { sample_rate_hz: 4000, max_record_s: 30 },
  { sample_rate_hz: 5000, max_record_s: 25 },
];

function acquisition(overrides: Partial<DeviceAcquisitionStatus> = {}): DeviceAcquisitionStatus {
  return {
    running: false, requested_running: false, hardware_ready: true, source: 'AD7606', sample_rate_hz: 0,
    selected_sample_rate_hz: 500, selected_record_limit_ms: 0, sample_rate_options: OPTIONS,
    samples_recorded: 0, recording_ms: 0, ...overrides,
  };
}

describe.each(['en', 'uk'] as const)('sample rate select %s', (locale) => {
  const t = (key: Parameters<typeof translate>[1], values?: Record<string, string | number>) =>
    translate(locale, key, values);

  it('offers every preset with its longest recording and no limit at 500 Hz', () => {
    renderUi(<SampleRateSelect acquisition={acquisition()} busy={false} locked={false} onSelect={vi.fn()} />, locale);
    const select = screen.getByLabelText(t('objects.rate_label')) as HTMLSelectElement;
    expect(select.value).toBe('500');
    const labels = Array.from(select.options).map((option) => option.textContent);
    expect(labels).toHaveLength(4);
    expect(labels[0]).toContain(t('objects.rate_option_unlimited', { rate: '500' }).replace(/\u00a0/g, ' ').slice(0, 3));
    expect(labels[1]).toMatch(/120/);
    expect(labels[2]).toMatch(/30/);
    expect(labels[3]).toMatch(/25/);
    expect(screen.getByText(t('objects.rate_note'))).toBeTruthy();
  });

  it('reports the chosen rate as a number', async () => {
    const user = userEvent.setup();
    const onSelect = vi.fn();
    renderUi(<SampleRateSelect acquisition={acquisition()} busy={false} locked={false} onSelect={onSelect} />, locale);
    await user.selectOptions(screen.getByLabelText(t('objects.rate_label')), '4000');
    expect(onSelect).toHaveBeenCalledExactlyOnceWith(4000);
  });

  it('is locked with an explanation while the live signals or a recording run', () => {
    renderUi(
      <SampleRateSelect acquisition={acquisition({ running: true, sample_rate_hz: 2000, selected_sample_rate_hz: 2000 })}
        busy={false} locked onSelect={vi.fn()} />, locale);
    const select = screen.getByLabelText(t('objects.rate_label')) as HTMLSelectElement;
    expect(select.disabled).toBe(true);
    expect(select.value).toBe('2000');
    expect(screen.getByText(t('objects.rate_stop_first'))).toBeTruthy();
  });

  it('is disabled while a control request is in flight', () => {
    renderUi(<SampleRateSelect acquisition={acquisition()} busy locked={false} onSelect={vi.fn()} />, locale);
    expect((screen.getByLabelText(t('objects.rate_label')) as HTMLSelectElement).disabled).toBe(true);
  });

  it('renders nothing when the firmware offers no presets', () => {
    const { container } = renderUi(
      <SampleRateSelect acquisition={acquisition({ sample_rate_options: undefined })} busy={false} locked={false}
        onSelect={vi.fn()} />, locale);
    expect(container.querySelector('select')).toBeNull();
  });
});

type ObjectsProps = ComponentProps<typeof ObjectsFeature>;
function objectsProps(rate: Partial<DeviceAcquisitionStatus>): ObjectsProps {
  const status = readyStatus();
  status.acquisition = { ...status.acquisition, ...acquisition(rate) };
  status.workflow = { ...status.workflow!, step: 'RECORD_READY' };
  return {
    status, store: status.object_store!, sessions: null, selectedId: 'OBJ_0001', busy: false, error: null,
    onSelect: vi.fn(), onCreate: vi.fn(async () => true), onActivate: vi.fn(async () => {}),
    onRename: vi.fn(async () => {}), onDelete: vi.fn(async () => {}),
    onBindGps: vi.fn(async () => ({ latitude: 0, longitude: 0, accuracy_m: 1, captured_at_ms: 1,
      altitude_m: null, altitude_accuracy_m: null })),
    onCable: vi.fn(async () => {}), onDefaultCable: vi.fn(async () => {}),
    onAcquisition: vi.fn(async () => {}), onRecording: vi.fn(async () => {}),
    onSampleRate: vi.fn(async () => {}),
    onStartWorkflow: vi.fn(), onOpenScanner: vi.fn(),
  };
}

describe('objects recording card sample rate', () => {
  it('sends the chosen preset while the source is stopped', async () => {
    const user = userEvent.setup();
    const props = objectsProps({});
    renderUi(<ObjectsFeature {...props} />, 'en');
    await user.selectOptions(screen.getByLabelText(translate('en', 'objects.rate_label')), '5000');
    expect(props.onSampleRate).toHaveBeenCalledExactlyOnceWith(5000);
  });

  it('locks the selector while the source runs', () => {
    const props = objectsProps({ running: true, sample_rate_hz: 500 });
    renderUi(<ObjectsFeature {...props} />, 'en');
    expect((screen.getByLabelText(translate('en', 'objects.rate_label')) as HTMLSelectElement).disabled).toBe(true);
  });
});

// STOPPED loads the material settings of the object; the other steps here make no HTTP request.
function modelStoppedSettings() {
  respondJson('/api/localization/runs?job=settings&object_id=OBJ_0001', {
    ok: true, settings: null, presets: [],
    standard_processing: {
      timing_uncertainty_us: 2, noise_window_samples: 100, sta_window_samples: 20,
      lta_window_samples: 200, sta_lta_trigger_ratio: 3, refine_lookback_samples: 20,
      refine_lookahead_samples: 20, refine_amplitude_fraction: 0.5,
      snr_signal_window_samples: 40, minimum_snr: 2, semblance_window_half_span_samples: 8,
      s_pick_enabled: false, time_budget_s: 60,
    },
  });
}

describe.each(['en', 'uk'] as const)('guided workflow sample rate %s', (locale) => {
  /* The UK catalog loads lazily: ask for the text only once the component has rendered. */
  const label = () => translate(locale, 'objects.rate_label');

  function workflowProps(step: string, running: boolean) {
    const status = readyStatus();
    Object.assign(status.workflow!, { step, allowed_actions: ['START_RECORDING', 'BACK'], generation: 3 });
    status.acquisition = { ...status.acquisition, ...acquisition({ running, sample_rate_hz: running ? 500 : 0 }) };
    return {
      status, objectName: 'Quarry',
      sendCommand: vi.fn(async () => ({ ok: true as const, workflow: status.workflow! })),
      onRefreshStatus: vi.fn(async () => true), onWorkflowUpdate: vi.fn(), onOpenScanner: vi.fn(),
      onSampleRate: vi.fn(async () => {}),
    };
  }

  it('offers the presets on the record-ready step and sends the chosen one', async () => {
    const user = userEvent.setup();
    const props = workflowProps('RECORD_READY', false);
    renderUi(<WorkflowPanel {...props} />, locale);
    await user.selectOptions(screen.getByLabelText(label()), '2000');
    expect(props.onSampleRate).toHaveBeenCalledExactlyOnceWith(2000);
  });

  it('is locked while the live signals run', () => {
    renderUi(<WorkflowPanel {...workflowProps('RECORD_READY', true)} />, locale);
    expect((screen.getByLabelText(label()) as HTMLSelectElement).disabled).toBe(true);
  });

  it('is offered on no other step', () => {
    modelStoppedSettings();
    for (const step of ['RANGING_RESULT', 'RECORDING', 'STOPPED']) {
      const view = renderUi(<WorkflowPanel {...workflowProps(step, false)} />, locale);
      expect(screen.queryByLabelText(label())).toBeNull();
      view.unmount();
    }
  });

  it('says why a recording ended by itself', () => {
    modelStoppedSettings();
    const props = workflowProps('STOPPED', false);
    props.status.measurement = { ...props.status.measurement!, stopped_by_limit: true, state: 'REVIEW' };
    renderUi(<WorkflowPanel {...props} />, locale);
    expect(screen.getByText(translate(locale, 'objects.rate_stopped_by_limit'))).toBeTruthy();
  });
});
