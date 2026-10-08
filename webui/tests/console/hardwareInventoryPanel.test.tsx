import { beforeAll, describe, expect, it } from 'vitest';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

import HardwareInventoryPanel from '../../src/features/dashboard/HardwareInventoryPanel';
import {
  normalizeHardwareModule,
  reasonCodeFromLegacyReason,
} from '../../src/features/dashboard/hardwareInventoryModel';
import type {
  DeviceStatus,
  HardwareModuleId,
  HardwareModuleState,
  HardwareReasonCode,
} from '../../src/domain/device';
import { loadUkrainianCatalog, LocaleProvider, translate, type I18nKey, type I18nLocale } from '../../src/i18n';

function render(status: DeviceStatus | null, locale: I18nLocale = 'en'): string {
  return renderToStaticMarkup(createElement(
    LocaleProvider,
    { initialLocale: locale },
    createElement(HardwareInventoryPanel, { status }),
  ));
}

function statusWithModule(module: Record<string, unknown>): DeviceStatus {
  return {
    hardware: { modules: [module], required_missing: false },
  } as unknown as DeviceStatus;
}

const REASON_TRANSLATIONS = [
  { code: 'ready', en: 'Ready', uk: 'Готово' },
  { code: 'no_response', en: 'No response', uk: 'Немає відповіді' },
  { code: 'no_filesystem', en: 'No filesystem', uk: 'Немає файлової системи' },
  { code: 'mount_failed', en: 'Mount failed', uk: 'Не вдалося змонтувати' },
  { code: 'read_probe_failed', en: 'Read probe failed', uk: 'Помилка перевірки читання' },
  { code: 'topology_probe_ok', en: 'Topology probe OK', uk: 'Перевірка топології успішна' },
  { code: 'topology_probe_fault', en: 'Topology probe fault', uk: 'Збій перевірки топології' },
  { code: 'no_upstream_response', en: 'No upstream response', uk: 'Немає відповіді від головного вузла' },
  { code: 'sensor_invalid', en: 'Sensor invalid', uk: 'Датчик недійсний' },
  { code: 'first_sample_ok', en: 'First sample OK', uk: 'Перший відлік отримано' },
  { code: 'no_valid_sample', en: 'No valid sample', uk: 'Немає дійсних відліків' },
  { code: 'busy_responds', en: 'BUSY line responds', uk: 'Лінія BUSY відповідає' },
  { code: 'no_busy_response', en: 'No BUSY response', uk: 'Немає відповіді BUSY' },
  { code: 'presence_probe_failed', en: 'Presence probe failed', uk: 'Помилка перевірки наявності' },
  { code: 'blocked_5v_rail_low', en: 'Blocked: 5V rail low', uk: 'Заблоковано: низька напруга 5 В' },
  { code: 'init_failed', en: 'Init failed', uk: 'Помилка ініціалізації' },
  { code: 'start_failed', en: 'Start failed', uk: 'Помилка запуску' },
  { code: 'sensor_ok', en: 'Sensor ready', uk: 'Датчик готовий' },
  { code: 'upstream_target_assigned', en: 'Main node assigned', uk: 'Головний вузол призначено' },
  { code: 'upstream_topology_fault', en: 'Upstream topology fault', uk: 'Збій топології головного вузла' },
  { code: 'none', en: 'No details', uk: 'Без подробиць' },
  { code: 'unrecognized', en: 'Unrecognized reason', uk: 'Нерозпізнана причина' },
] as const;

const LEGACY_REASON_CASES = [
  { reason: 'ready', code: 'ready' },
  { reason: 'no response', code: 'no_response' },
  { reason: 'no filesystem', code: 'no_filesystem' },
  { reason: 'mount failed', code: 'mount_failed' },
  { reason: 'read probe failed', code: 'read_probe_failed' },
  { reason: 'topology probe ok', code: 'topology_probe_ok' },
  { reason: 'topology probe fault', code: 'topology_probe_fault' },
  { reason: 'no upstream response', code: 'no_upstream_response' },
  { reason: 'sensor invalid', code: 'sensor_invalid' },
  { reason: 'first sample ok', code: 'first_sample_ok' },
  { reason: 'no valid sample', code: 'no_valid_sample' },
  { reason: 'busy responds', code: 'busy_responds' },
  { reason: 'no BUSY response', code: 'no_busy_response' },
  { reason: 'presence probe failed', code: 'presence_probe_failed' },
  { reason: 'blocked: 5V rail low', code: 'blocked_5v_rail_low' },
  { reason: 'init failed', code: 'init_failed' },
  { reason: 'start failed', code: 'start_failed' },
  { reason: 'sensor ok', code: 'sensor_ok' },
  { reason: 'upstream target assigned', code: 'upstream_target_assigned' },
  { reason: 'upstream topology fault', code: 'upstream_topology_fault' },
] as const;

beforeAll(async () => { await loadUkrainianCatalog(); });

describe('hardware inventory dashboard panel', () => {
  it.each(['en', 'uk'] as const)('shows module state and requirement in %s', (locale) => {
    const status = {
      hardware: {
        modules: [
          { id: 'sd_card', state: 'present', required: true, reason: 'ready', reason_code: 'ready' },
          {
            id: 'barometer', state: 'degraded', required: false,
            reason: 'sensor ok', reason_code: 'sensor_ok',
          },
        ],
        required_missing: true,
      },
    } as unknown as DeviceStatus;

    const html = render(status, locale);
    expect(html).toContain(translate(locale, 'web.hw.title' as I18nKey));
    expect(html).toContain(translate(locale, 'web.hw.module.sd_card' as I18nKey));
    expect(html).toContain(translate(locale, 'web.hw.state.present' as I18nKey));
    expect(html).toContain(translate(locale, 'web.hw.required' as I18nKey));
    expect(html).toContain(translate(locale, 'web.hw.optional' as I18nKey));
    expect(html).toContain(translate(locale, 'web.hw.degraded_banner' as I18nKey));
    expect(html).toContain(translate(locale, 'web.hw.reason.ready' as I18nKey));
    expect(html).toContain(translate(locale, 'web.hw.reason.sensor_ok' as I18nKey));
    expect(html).not.toContain('mounted');
    expect(html).not.toContain('slow response');
  });

  it.each(REASON_TRANSLATIONS)('renders the Ukrainian reason for $code', ({ code, uk }) => {
    const rawReason = code === 'none' ? '' : `raw-${code}`;
    const html = render(statusWithModule({
      id: 'sd_card', state: 'present', required: true, reason: rawReason, reason_code: code,
    }), 'uk');

    expect(html).toContain(uk);
    if (code === 'unrecognized') {
      expect(html).toContain(rawReason);
    } else if (rawReason.length > 0) {
      expect(html).not.toContain(rawReason);
    }
  });

  it('renders an English translated reason without the diagnostic detail', () => {
    const html = render(statusWithModule({
      id: 'sd_card', state: 'present', required: true,
      reason: 'read probe failed', reason_code: 'read_probe_failed',
    }));

    expect(html).toContain('Read probe failed');
    expect(html).not.toContain('read probe failed');
  });

  it('shows unknown reason codes as translated unrecognized text with muted raw detail', () => {
    const html = render(statusWithModule({
      id: 'sd_card', state: 'fault', required: true,
      reason: 'brand new diagnostic', reason_code: 'brand_new_code',
    }));

    expect(html).toContain('Unrecognized reason');
    expect(html).toContain('hardware-module-reason-diagnostics');
    expect(html).toContain('brand new diagnostic');
  });

  it.each([
    { reason: 'no response', code: 'no_response' },
    { reason: 'mount failed', code: 'mount_failed' },
    { reason: 'start failed: ESP_ERR_TIMEOUT', code: 'start_failed' },
  ] as const)('translates legacy reason "$reason" without showing it raw', ({ reason, code }) => {
    const expected = REASON_TRANSLATIONS.find((entry) => entry.code === code);
    expect(expected).toBeDefined();
    const html = render(statusWithModule({
      id: 'sd_card', state: 'fault', required: true, reason,
    }), 'uk');

    expect(html).toContain(expected!.uk);
    expect(html).not.toContain(reason);
  });

  it('shows an unknown legacy reason as unrecognized and keeps it as diagnostics', () => {
    const html = render(statusWithModule({
      id: 'sd_card', state: 'fault', required: true, reason: 'brand new legacy detail',
    }), 'uk');

    expect(html).toContain('Нерозпізнана причина');
    expect(html).toContain('hardware-module-reason-diagnostics');
    expect(html).toContain('brand new legacy detail');
  });

  it.each([
    ['en', 'Unrecognized'],
    ['uk', 'Не розпізнано'],
  ] as const)('labels a typo state as unrecognized in %s', (locale, expected) => {
    const html = render(statusWithModule({
      id: 'sd_card', state: 'degradded', required: true, reason: 'ready', reason_code: 'ready',
    }), locale);

    expect(html).toContain(expected);
    expect(html).not.toContain(translate(locale, 'web.hw.state.unknown' as I18nKey));
    expect(html).toContain('hardware-state-unrecognized');
    expect(html).not.toContain('degradded');
  });

  it('renders the raw identifier for an unknown module id', () => {
    const html = render(statusWithModule({
      id: 'custom_board', state: 'present', required: false, reason: '',
    }));

    expect(html).toContain('custom_board');
  });

  it.each([
    ['en', 'Not tested'],
    ['uk', 'Не тестовано'],
  ] as const)('labels an unknown inventory state as not tested in %s', (locale, expected) => {
    const status = {
      hardware: {
        modules: [{ id: 'ad7606', state: 'unknown', required: true, reason: '' }],
        required_missing: false,
      },
    } as unknown as DeviceStatus;

    const html = render(status, locale);
    expect(translate(locale, 'web.hw.state.unknown' as I18nKey)).toBe(expected);
    expect(html).toContain(expected);
  });

  it('does not show the panel when older firmware omits hardware', () => {
    const html = render({} as DeviceStatus);
    expect(html).not.toContain('hardware-inventory-card');
  });
});

describe('hardware inventory normalization', () => {
  it('returns closed known unions for known module, state and reason code', () => {
    const module = normalizeHardwareModule({
      id: 'sd_card', state: 'present', reason: 'ready', reason_code: 'ready',
    });
    const moduleId: HardwareModuleId | null = module.knownId ? module.id : null;
    const state: HardwareModuleState | 'unrecognized' = module.state;
    const code: HardwareReasonCode = module.reasonCode;

    expect({ moduleId, knownId: module.knownId, state, code, rawReason: module.rawReason }).toEqual({
      moduleId: 'sd_card', knownId: true, state: 'present', code: 'ready', rawReason: 'ready',
    });
  });

  it.each(LEGACY_REASON_CASES)('maps legacy reason "$reason" to $code', ({ reason, code }) => {
    expect(reasonCodeFromLegacyReason(reason)).toBe(code);
    expect(normalizeHardwareModule({ id: 'sd_card', state: 'present', reason }).reasonCode).toBe(code);
  });

  it('maps start-failure suffixes, empty reasons, unknown reasons and typo values explicitly', () => {
    expect(reasonCodeFromLegacyReason('start failed: ESP_ERR_TIMEOUT')).toBe('start_failed');
    expect(reasonCodeFromLegacyReason('start failed: vendor specific')).toBe('start_failed');
    expect(reasonCodeFromLegacyReason('')).toBe('none');
    expect(reasonCodeFromLegacyReason('   ')).toBe('none');
    expect(reasonCodeFromLegacyReason('totally new text')).toBe('unrecognized');
    expect(normalizeHardwareModule({ id: 'sd_card', state: 'present' }).reasonCode).toBe('none');
    expect(normalizeHardwareModule({ id: 'custom_board', state: 'degradded', reason: '' })).toMatchObject({
      id: 'custom_board', knownId: false, state: 'unrecognized', reasonCode: 'none',
    });
    expect(normalizeHardwareModule({
      id: 'sd_card', state: 'present', reason: 'ready', reason_code: 'brand_new_code',
    }).reasonCode).toBe('unrecognized');
  });
});
