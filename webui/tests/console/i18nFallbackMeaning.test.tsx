import { beforeAll, describe, expect, it } from 'vitest';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

import PowerBanner from '../../src/components/PowerBanner';
import type { DeviceStatus } from '../../src/domain/device';
import { jobProgressText, type LocalizationJobStatus } from '../../src/domain/localizationJob';
import { loadUkrainianCatalog, LocaleProvider, translate, type I18nKey } from '../../src/i18n';
import { uk } from '../../src/i18n/catalog.uk.gen';
import LocalizationJobReport from '../../src/features/objects/LocalizationJobReport';

beforeAll(async () => { await loadUkrainianCatalog(); });

function withMissingUkrainianKey(key: I18nKey, check: () => void): void {
  const original = uk[key];
  expect(Reflect.deleteProperty(uk, key)).toBe(true);
  try {
    check();
  } finally {
    uk[key] = original;
  }
}

function jobStatus(): LocalizationJobStatus {
  return {
    state: 'running',
    job_id: 42,
    object_id: 'OBJ_0001',
    session_id: 'S000001',
    computed_at_ms: 100,
    current_method: 'tdcas',
    elapsed_ms: 1250,
    budget_ms: 60000,
    summary: {
      participating_channel_count: 4,
      methods_attempted: 1,
      methods_solved: 0,
      methods_persisted: 0,
      methods_skipped_for_time: 0,
      timings: [],
    },
    stack: { size_bytes: 8192, free_min_bytes: 4096, margin_bytes: 1024, margin_ok: true },
  };
}

describe('missing Ukrainian catalog entries preserve message meaning', () => {
  it('falls back to English battery prose while keeping alert severity and low-battery state', () => {
    const key = 'web.power.low';
    const ukrainian = uk[key];
    const status = {
      power: { valid: true, state: 'BATTERY_LOW', low_battery: 'warning' },
    } as unknown as DeviceStatus;

    withMissingUkrainianKey(key, () => {
      const html = renderToStaticMarkup(createElement(LocaleProvider, { initialLocale: 'uk' },
        createElement(PowerBanner, { status }),
      ));
      expect(html).toContain(`role="alert">${translate('en', key)}</div>`);
      expect(html).not.toContain(ukrainian);
    });
  });

  it('falls back to English job prose without changing state, method code, or reported times', () => {
    const key = 'calc.job_running';
    const ukrainian = uk[key];
    const status = jobStatus();

    withMissingUkrainianKey(key, () => {
      const html = renderToStaticMarkup(createElement(LocaleProvider, { initialLocale: 'uk' },
        createElement(LocalizationJobReport, { status }),
      ));
      expect(html).toContain(`data-job-state="running">${jobProgressText(status, 'uk')}</p>`);
      expect(html).toContain('tdcas');
      expect(html).toContain('1,3');
      expect(html).toContain('60,0');
      expect(html).not.toContain(ukrainian);
    });
  });
});
