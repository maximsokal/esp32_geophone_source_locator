import { afterEach, beforeAll, describe, expect, it, vi } from 'vitest';
import { createElement } from 'react';
import { act, create, type ReactTestRenderer } from 'react-test-renderer';
import type { DeviceStatus, ObjectStoreState } from '../../src/domain/device';
import ObjectsFeature from '../../src/features/objects/ObjectsFeature';
import { normalizeDeviceStatus } from '../../src/api/deviceStatusSchema';
import { fullDeviceStatus } from './fixtures/deviceStatusFull';
import { loadUkrainianCatalog, LocaleProvider, translate, type I18nLocale } from '../../src/i18n';

const profile = (id: string, name: string, active: boolean, scopeAvailable = true) => ({
  id, name, active, cable_lengths_m: [5, 5, 5, 5],
  location: { valid: false, latitude: 0, longitude: 0, accuracy_m: 0, altitude_valid: false,
    altitude_m: 0, altitude_accuracy_m: 0, captured_at_ms: 0, source: '' },
  delete_scope: { available: scopeAvailable, sessions: 3, artifacts: 4, localization_runs: 2 },
});

const store: ObjectStoreState = {
  available: true, active_id: 'OBJ_A', default_cable_lengths_m: [5, 5, 5, 5],
  objects: [profile('OBJ_A', 'Object A', true), profile('OBJ_B', 'Object B', false)],
};

function status(recordingObjectId: string | null): DeviceStatus {
  const value = fullDeviceStatus();
  value.recording = { available: true, running: recordingObjectId !== null, state: recordingObjectId ? 'RECORDING' : 'IDLE',
    object_id: recordingObjectId ?? undefined, session_id: recordingObjectId ? 'S000009' : undefined };
  value.measurement = { available: true, state: recordingObjectId ? 'RECORDING' : 'LIVE',
    object_id: recordingObjectId ?? undefined, object_name: recordingObjectId ? 'Object A' : undefined,
    session_id: recordingObjectId ? 'S000009' : undefined };
  return value;
}

function props(value: DeviceStatus, selectedId = 'OBJ_B') {
  return {
    status: value, store, sessions: null, selectedId, busy: false, error: null,
    onSelect: vi.fn(), onCreate: vi.fn(async () => undefined), onActivate: vi.fn(async () => undefined),
    onRename: vi.fn(async () => undefined), onDelete: vi.fn(async () => undefined),
    onBindGps: vi.fn(async () => ({ latitude: 0, longitude: 0, accuracy_m: 1, captured_at_ms: 1, source: 'test' })),
    onCable: vi.fn(async () => undefined), onDefaultCable: vi.fn(async () => undefined),
    onAcquisition: vi.fn(async () => undefined), onRecording: vi.fn(async () => undefined),
    onStartWorkflow: vi.fn(), onOpenScanner: vi.fn(),
  };
}

async function render(value: DeviceStatus, locale: I18nLocale, selectedId = 'OBJ_B') {
  let renderer!: ReactTestRenderer;
  await act(async () => {
    renderer = create(createElement(LocaleProvider, { initialLocale: locale },
      createElement(ObjectsFeature, props(value, selectedId))));
    await Promise.resolve();
  });
  return renderer;
}

function button(renderer: ReactTestRenderer, locale: I18nLocale, key: string) {
  const label = translate(locale, key);
  const found = renderer.root.findAll((node) => node.type === 'button' && node.children.join('') === label);
  expect(found.length).toBeGreaterThan(0);
  return found[0];
}

beforeAll(async () => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  await loadUkrainianCatalog();
});
afterEach(() => { vi.unstubAllGlobals(); vi.restoreAllMocks(); });

describe('MAX-459 object deletion and recording locks', () => {
  it('validates the additive delete-scope counters in the status API shape', () => {
    const malformed = fullDeviceStatus();
    const objectProfile = malformed.object_store!.objects[0] as unknown as { delete_scope: unknown };
    objectProfile.delete_scope = { available: true, sessions: 'three', artifacts: 0, localization_runs: 0 };
    expect(() => normalizeDeviceStatus(malformed)).toThrow();
  });


  it.each(['en', 'uk'] as const)('includes deletion scope counts in %s confirmation', async (locale) => {
    const confirm = vi.fn(() => true);
    vi.stubGlobal('window', { confirm });
    const componentProps = props(status(null));
    let renderer!: ReactTestRenderer;
    await act(async () => {
      renderer = create(createElement(LocaleProvider, { initialLocale: locale },
        createElement(ObjectsFeature, componentProps)));
      await Promise.resolve();
    });
    await act(async () => button(renderer, locale, 'web.objects.delete').props.onClick());
    expect(confirm).toHaveBeenCalledWith(translate(locale, 'objects.delete_confirm_scope', {
      name: 'Object B', sessions: '3', artifacts: '4', runs: '2',
    }));
    expect(componentProps.onDelete).toHaveBeenCalledWith('OBJ_B');
    act(() => renderer.unmount());
  });

  it.each(['en', 'uk'] as const)('locks another object while a recording is active and explains why in %s', async (locale) => {
    const renderer = await render(status('OBJ_A'), locale);
    const renameInput = renderer.root.findAll((node) => node.type === 'input' && node.props.value === 'Object B')[0];
    act(() => renameInput.props.onChange({ target: { value: 'Changed B' } }));
    expect(renameInput.props.disabled).toBe(true);
    expect(button(renderer, locale, 'objects.rename').props.disabled).toBe(true);
    expect(button(renderer, locale, 'web.objects.set_active').props.disabled).toBe(true);
    expect(button(renderer, locale, 'web.objects.delete').props.disabled).toBe(true);
    expect(button(renderer, locale, 'objects.bind_gps').props.disabled).toBe(true);
    expect(button(renderer, locale, 'objects.save').props.disabled).toBe(true);
    const cableInputs = renderer.root.findAll((node) => node.type === 'input' && node.props.type === 'number' && node.props.disabled === true);
    expect(cableInputs.length).toBe(4);
    const text = renderer.root.findAll((node) => node.props.role === 'status').map((node) => node.children.join('')).join(' ');
    expect(text).toContain(translate(locale, 'objects.controls_locked_recording', { object: 'Object A', session: 'S000009' }));
    act(() => renderer.unmount());

    const activeObjectRenderer = await render(status('OBJ_A'), locale, 'OBJ_A');
    expect(button(activeObjectRenderer, locale, 'web.objects.delete').props.disabled).toBe(true);
    act(() => activeObjectRenderer.unmount());
  });
});
