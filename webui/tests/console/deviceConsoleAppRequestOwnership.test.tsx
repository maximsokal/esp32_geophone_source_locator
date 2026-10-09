import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createElement } from 'react';
import { act, create, type ReactTestRenderer } from 'react-test-renderer';

const stateUpdates = vi.hoisted(() => ({ count: 0 }));
vi.mock('react', async (importOriginal) => {
  const actual = await importOriginal<typeof import('react')>();
  return {
    ...actual,
    useState: <T,>(initial: T | (() => T)) => {
      const [value, setValue] = actual.useState(initial);
      return [value, (next: import('react').SetStateAction<T>) => {
        stateUpdates.count += 1;
        setValue(next);
      }] as const;
    },
  };
});

import DeviceConsoleApp from '../../src/DeviceConsoleApp';
import { deviceApiHasPendingRequest, fetchDeviceStatus } from '../../src/api/deviceApi';
import { LocaleProvider } from '../../src/i18n';
import { fullDeviceStatus } from './fixtures/deviceStatusFull';

function deferred<T>() {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((resolvePromise) => { resolve = resolvePromise; });
  return { promise, resolve };
}

function jsonResponse(body: unknown): Response {
  return new Response(JSON.stringify(body), { status: 200, headers: { 'Content-Type': 'application/json' } });
}

async function settlePromises(): Promise<void> {
  for (let turn = 0; turn < 10; turn += 1) await Promise.resolve();
}

async function mountApp(): Promise<ReactTestRenderer> {
  let renderer!: ReactTestRenderer;
  await act(async () => {
    renderer = create(createElement(LocaleProvider, { initialLocale: 'en' }, createElement(DeviceConsoleApp)));
    await settlePromises();
  });
  return renderer;
}

function unmount(renderer: ReactTestRenderer): void {
  act(() => renderer.unmount());
}

beforeEach(() => {
  stateUpdates.count = 0;
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  vi.stubGlobal('window', {
    localStorage: { getItem: () => null, setItem: vi.fn() },
    setTimeout: (callback: () => void, delay = 0) => globalThis.setTimeout(callback, delay),
    clearTimeout: (timer: number) => globalThis.clearTimeout(timer),
    setInterval: (callback: () => void, delay = 0) => globalThis.setInterval(callback, delay),
    clearInterval: (timer: number) => globalThis.clearInterval(timer),
    confirm: () => false,
    addEventListener: vi.fn(),
    removeEventListener: vi.fn(),
  });
  vi.stubGlobal('document', {
    hidden: false,
    visibilityState: 'visible',
    addEventListener: vi.fn(),
    removeEventListener: vi.fn(),
  });
  vi.stubGlobal('navigator', { onLine: true });
});

afterEach(() => {
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

describe('DeviceConsoleApp request ownership', () => {
  it('removes its queued objects status read before it can reach the server on unmount', async () => {
    const blockerResponse = deferred<Response>();
    const starts: string[] = [];
    const fetchMock = vi.fn((url: string) => {
      starts.push(url);
      if (url === '/api/status') return blockerResponse.promise;
      return Promise.resolve(jsonResponse(fullDeviceStatus()));
    });
    vi.stubGlobal('fetch', fetchMock);

    const blockerRead = fetchDeviceStatus(false, undefined, { preemptible: false });
    const renderer = await mountApp();
    const startsBeforeUnmount = [...starts];
    expect(deviceApiHasPendingRequest()).toBe(true);

    const updatesBeforeUnmount = stateUpdates.count;
    unmount(renderer);
    const updatesAfterUnmount = stateUpdates.count;
    blockerResponse.resolve(jsonResponse(fullDeviceStatus()));
    await act(async () => {
      await blockerRead;
      await settlePromises();
    });

    expect(startsBeforeUnmount).toEqual(['/api/status']);
    expect(starts).toEqual(['/api/status']);
    expect(deviceApiHasPendingRequest()).toBe(false);
    expect(stateUpdates.count).toBe(updatesAfterUnmount);
    expect(updatesAfterUnmount).toBe(updatesBeforeUnmount);
  });

  it('aborts an active status read on unmount and ignores its late response', async () => {
    const lateResponse = deferred<Response>();
    let statusSignal: AbortSignal | undefined;
    const starts: string[] = [];
    const fetchMock = vi.fn((url: string, init?: RequestInit) => {
      starts.push(url);
      statusSignal = init?.signal as AbortSignal;
      return lateResponse.promise;
    });
    vi.stubGlobal('fetch', fetchMock);

    const renderer = await mountApp();
    const updatesBeforeUnmount = stateUpdates.count;
    unmount(renderer);
    const wasAborted = statusSignal?.aborted ?? false;
    const updatesAfterUnmount = stateUpdates.count;
    lateResponse.resolve(jsonResponse(fullDeviceStatus()));
    await act(async () => { await settlePromises(); });

    expect(starts).toEqual(['/api/status?objects=1']);
    expect(wasAborted).toBe(true);
    expect(stateUpdates.count).toBe(updatesAfterUnmount);
    expect(updatesAfterUnmount).toBe(updatesBeforeUnmount);
  });
});
