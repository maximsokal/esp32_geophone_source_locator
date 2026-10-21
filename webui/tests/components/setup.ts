import { createElement, type ReactElement } from 'react';
import { cleanup, render } from '@testing-library/react';
import { afterEach, beforeAll, beforeEach, expect, vi } from 'vitest';
import { LocaleProvider, loadUkrainianCatalog, type I18nLocale } from '../../src/i18n';
import { normalizeDeviceStatus } from '../../src/api/deviceStatusSchema';
import { fullDeviceStatus } from '../console/fixtures/deviceStatusFull';

const unexpectedRequests: string[] = [];
const consoleErrors: string[] = [];
const jsonResponses = new Map<string, unknown>();

beforeAll(loadUkrainianCatalog);
beforeEach(() => {
  localStorage.clear();
  unexpectedRequests.length = 0;
  consoleErrors.length = 0;
  jsonResponses.clear();
  vi.spyOn(console, 'error').mockImplementation((...args) => { consoleErrors.push(args.map(String).join(' ')); });
  vi.stubGlobal('fetch', vi.fn(async (input: unknown, init?: RequestInit) => {
    if ((init?.method ?? 'GET') === 'GET' && jsonResponses.has(String(input))) {
      return Response.json(jsonResponses.get(String(input)));
    }
    unexpectedRequests.push(String(input));
    throw new Error(`Unexpected component-test HTTP: ${String(input)}`);
  }));
});

export function respondJson(url: string, body: unknown): void {
  jsonResponses.set(url, body);
}
afterEach(() => {
  cleanup();
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
  expect(unexpectedRequests, 'Component tests must explicitly model their HTTP boundary').toEqual([]);
  expect(consoleErrors, 'React/DOM errors must not be swallowed').toEqual([]);
});

export function renderUi(ui: ReactElement, locale: I18nLocale = 'en') {
  return render(ui, {
    wrapper: ({ children }) => createElement(LocaleProvider, { initialLocale: locale }, children),
    reactStrictMode: true,
  });
}

export function readyStatus() {
  const status = fullDeviceStatus();
  status.hardware!.modules = [
    { id: 'ad7606', state: 'present', required: true, reason: 'ready', reason_code: 'ready' },
  ];
  // These component scenarios have no log-capable nodes; browser smoke covers real log reads.
  status.nrf_logs = [];
  return normalizeDeviceStatus(status);
}
