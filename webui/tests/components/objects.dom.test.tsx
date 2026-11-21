import type { ComponentProps } from 'react';
import { act, screen, waitFor, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { afterEach, describe, expect, it, vi } from 'vitest';
/* The evidence card of an opened session talks to the device (scan info); this file models only the scanner entry. */
vi.mock('../../src/features/objects/RecordingEvidenceCard', () => ({ default: () => null }));
import ObjectsFeature from '../../src/features/objects/ObjectsFeature';
import DeviceConsoleApp from '../../src/DeviceConsoleApp';
import { translate } from '../../src/i18n';
import { clearScanLaunchIntent, hasScanLaunchIntent } from '../../src/xr/scanLaunchIntent';
import { readyStatus, renderUi, respondJson } from './setup';

type Props = ComponentProps<typeof ObjectsFeature>;
function objectProps(): Props {
  const status = readyStatus();
  status.object_store!.objects[0].delete_scope = { available: true, sessions: 3, artifacts: 4, localization_runs: 2 };
  return {
    status, store: status.object_store!, sessions: null, selectedId: 'OBJ_0001', busy: false, error: null,
    onSelect: vi.fn(), onCreate: vi.fn(async () => true), onActivate: vi.fn(async () => {}),
    onRename: vi.fn(async () => {}), onDelete: vi.fn(async () => {}),
    onBindGps: vi.fn(async () => ({ latitude: 0, longitude: 0, accuracy_m: 1, captured_at_ms: 1,
      altitude_m: null, altitude_accuracy_m: null })),
    onCable: vi.fn(async () => {}), onDefaultCable: vi.fn(async () => {}),
    onAcquisition: vi.fn(async () => {}), onRecording: vi.fn(async () => {}),
    onStartWorkflow: vi.fn(), onOpenScanner: vi.fn(),
  };
}

describe.each(['en', 'uk'] as const)('objects DOM %s', (locale) => {
  const t = (key: Parameters<typeof translate>[1]) => translate(locale, key);

  it('submits the typed name with Enter and clears the form only after success', async () => {
    const user = userEvent.setup();
    const props = objectProps();
    let finish!: () => void;
    vi.mocked(props.onCreate).mockImplementation(() => new Promise<boolean>((resolve) => { finish = () => resolve(true); }));
    renderUi(<ObjectsFeature {...props} />, locale);
    const name = screen.getByPlaceholderText(t('objects.new_name')) as HTMLInputElement;
    await user.type(name, '  New survey  {Enter}');
    expect(props.onCreate).toHaveBeenCalledExactlyOnceWith('New survey');
    expect(name.value).toBe('  New survey  ');
    await act(async () => { finish(); });
    expect(name.value).toBe('');
    expect(name.maxLength).toBe(31);
  });

  it('rejects empty/unchanged rename and dispatches one trimmed name', async () => {
    const user = userEvent.setup();
    const props = objectProps();
    renderUi(<ObjectsFeature {...props} />, locale);
    const name = screen.getByDisplayValue('Quarry');
    const rename = screen.getByRole('button', { name: t('objects.rename') }) as HTMLButtonElement;
    expect(rename.disabled).toBe(true);
    await user.clear(name);
    await user.type(name, '   ');
    await user.click(rename);
    expect(props.onRename).not.toHaveBeenCalled();
    await user.type(name, 'Renamed survey  ');
    await user.click(rename);
    expect(props.onRename).toHaveBeenCalledExactlyOnceWith('OBJ_0001', 'Renamed survey');
  });

  it('retains the entered name when the parent reports a rejected create', async () => {
    const user = userEvent.setup();
    const props = objectProps();
    vi.mocked(props.onCreate).mockResolvedValue(false);
    renderUi(<ObjectsFeature {...props} />, locale);
    const name = screen.getByRole('textbox', { name: t('objects.new_name') });
    await user.type(name, 'Retry survey{Enter}');
    await waitFor(() => expect(props.onCreate).toHaveBeenCalledExactlyOnceWith('Retry survey'));
    expect((name as HTMLInputElement).value).toBe('Retry survey');
  });

  it('respects native delete cancel/confirm and identifies the deletion scope', async () => {
    const user = userEvent.setup();
    const props = objectProps();
    const confirm = vi.spyOn(window, 'confirm').mockReturnValueOnce(false).mockReturnValueOnce(true);
    renderUi(<ObjectsFeature {...props} />, locale);
    const remove = screen.getByRole('button', { name: t('web.objects.delete') });
    await user.click(remove);
    expect(props.onDelete).not.toHaveBeenCalled();
    expect(confirm).toHaveBeenLastCalledWith(translate(locale, 'objects.delete_confirm_scope', {
      name: 'Quarry', sessions: '3', artifacts: '4', runs: '2',
    }));
    await user.keyboard(' ');
    expect(props.onDelete).toHaveBeenCalledExactlyOnceWith('OBJ_0001');
  });

  it('keeps busy controls inert and validates/clamps cable values at the existing boundary', async () => {
    const user = userEvent.setup();
    const props = objectProps();
    props.busy = true;
    const view = renderUi(<ObjectsFeature {...props} />, locale);
    for (const key of ['objects.create', 'objects.rename', 'web.objects.delete'] as const) {
      const button = screen.getByRole('button', { name: t(key) }) as HTMLButtonElement;
      expect(button.disabled).toBe(true);
      await user.click(button);
    }
    expect(props.onCreate).not.toHaveBeenCalled();
    expect(props.onRename).not.toHaveBeenCalled();
    expect(props.onDelete).not.toHaveBeenCalled();
    view.rerender(<ObjectsFeature {...props} busy={false} />);
    const card = screen.getByRole('heading', { name: t('objects.cable_lengths') }).closest('article')!;
    const length = within(card).getAllByRole('spinbutton')[0] as HTMLInputElement;
    await user.clear(length);
    await user.type(length, '101');
    expect(length.validity.rangeOverflow).toBe(true);
    await user.click(within(card).getAllByRole('button', { name: t('objects.save') })[0]);
    expect(props.onCable).toHaveBeenCalledExactlyOnceWith('OBJ_0001', 1, 100);
  });
});

it('production console admits only one create request while the response is pending', async () => {
  const user = userEvent.setup();
  const status = readyStatus();
  const controls: unknown[] = [];
  const unexpected: string[] = [];
  let finish!: (response: Response) => void;
  vi.stubGlobal('fetch', vi.fn(async (input: string, init?: RequestInit) => {
    if (input.startsWith('/api/status')) return Response.json(status);
    if (input === '/api/control' && init?.method === 'POST') {
      controls.push(JSON.parse(String(init.body)));
      return new Promise<Response>((resolve) => { finish = resolve; });
    }
    unexpected.push(input);
    throw new Error(`Unexpected integration HTTP: ${input}`);
  }));
  renderUi(<DeviceConsoleApp />);
  await screen.findByRole('heading', { name: 'Rescue locator console' });
  await user.click(within(screen.getByRole('navigation')).getByRole('button', { name: 'Objects' }));
  await screen.findByRole('heading', { name: 'Quarry' });
  await user.type(screen.getByPlaceholderText('New object name'), 'Survey');
  const create = screen.getByRole('button', { name: 'Create' }) as HTMLButtonElement;
  await user.dblClick(create);
  expect(controls).toEqual([{ action: 'create_object', name: 'Survey' }]);
  expect(create.disabled).toBe(true);
  await user.keyboard('{Enter}');
  expect(controls).toHaveLength(1);
  await act(async () => { finish(Response.json({ ok: true })); });
  await waitFor(() => expect(create.disabled).toBe(false));
  expect(unexpected).toEqual([]);
});

describe('one-tap scan entry from the objects list (issue #21)', () => {
  const navigatorWithActivation = navigator as Navigator & { userActivation?: { isActive: boolean } };
  afterEach(() => {
    clearScanLaunchIntent();
    delete navigatorWithActivation.userActivation;
  });

  function propsWithSession(): Props {
    return {
      ...objectProps(),
      sessions: {
        object_id: 'OBJ_0001', available: true,
        sessions: [{
          session_id: 'S000001', status: 'complete', complete: true, readable: true, source: 'AD7606',
          sample_rate_hz: 500, channel_count: 4, samples_written: 12000, dropped_frames: 0, recording_ms: 24000,
        }],
      },
    };
  }

  it.each([[true], [false]])('the "3D scan" tap opens the scanner once and leaves a launch intent only with a live activation (%s)', async (active) => {
    Object.defineProperty(navigator, 'userActivation', { configurable: true, value: { isActive: active } });
    respondJson('/api/localization/runs?object_id=OBJ_0001&session_id=S000001', { object_id: 'OBJ_0001', session_id: 'S000001', runs: [], geometry: { available: false, pods: [] } });
    respondJson('/api/seismic/session?object_id=OBJ_0001&session_id=S000001&start_frame=0&points=240', {});
    const user = userEvent.setup();
    const props = propsWithSession();
    renderUi(<ObjectsFeature {...props} />, 'en');
    await user.click(screen.getByRole('button', { name: /S000001/ }));
    expect(hasScanLaunchIntent()).toBe(false);                       /* opening a session is not a scan request */
    await user.click(screen.getByRole('button', { name: translate('en', 'objects.scan_add') }));
    expect(props.onOpenScanner).toHaveBeenCalledExactlyOnceWith('OBJ_0001', 'S000001');
    expect(hasScanLaunchIntent()).toBe(active);
  });
});
