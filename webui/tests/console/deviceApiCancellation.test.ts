import { afterEach, describe, expect, it, vi } from 'vitest';
import {
  ApiRequestOwnerRetiredError,
  ApiRequestPreemptedError,
  BulkTransferPreemptedError,
  DEVICE_API_FAST_TIMEOUT_MS,
  fetchDeviceStatus,
  fetchLocalizationRuns,
  isRequestCancellation,
  retryPreemptedRead,
} from '../../src/api/deviceApi';

afterEach(() => {
  vi.useRealTimers();
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

describe('device API request cancellation', () => {
  it('recognizes only the typed API read cancellation errors', async () => {
    expect(isRequestCancellation(new ApiRequestPreemptedError())).toBe(true);
    expect(isRequestCancellation(new ApiRequestOwnerRetiredError())).toBe(true);
    expect(isRequestCancellation(new BulkTransferPreemptedError())).toBe(false);
    expect(isRequestCancellation(new Error('offline'))).toBe(false);
    expect(isRequestCancellation(new Error(`ESP API timeout (GET /api/status, ${DEVICE_API_FAST_TIMEOUT_MS} ms)`))).toBe(false);
    expect(isRequestCancellation(null)).toBe(false);
    expect(isRequestCancellation(undefined)).toBe(false);
    expect(isRequestCancellation('ApiRequestPreemptedError')).toBe(false);
    expect(isRequestCancellation({ name: 'ApiRequestPreemptedError' })).toBe(false);

    vi.stubGlobal('fetch', vi.fn(async () => new Response(JSON.stringify({ error: 'bad request' }), {
      status: 400,
      statusText: 'Bad Request',
    })));
    const httpError = await fetchLocalizationRuns('OBJ_0001', 'S000001').catch((reason: unknown) => reason);
    expect(httpError).toMatchObject({ name: 'ApiHttpError' });
    expect(isRequestCancellation(httpError)).toBe(false);
  });

  it('does not classify a timed out read as cancellation', async () => {
    vi.useFakeTimers();
    vi.stubGlobal('fetch', vi.fn((_url: string, init?: RequestInit) => new Promise<Response>((_resolve, reject) => {
      init?.signal?.addEventListener('abort', () => reject(new DOMException('Aborted', 'AbortError')));
    })));

    const result = fetchDeviceStatus(false, undefined, { preemptible: false }).catch((reason: unknown) => reason);
    await vi.advanceTimersByTimeAsync(DEVICE_API_FAST_TIMEOUT_MS + 1);
    const timeout = await result;
    expect(timeout).toMatchObject({ message: expect.stringContaining('timeout') });
    expect(isRequestCancellation(timeout)).toBe(false);
  });

  it('retries preemption only and rethrows after the retry limit', async () => {
    const succeedsAfterPreemption = vi.fn()
      .mockRejectedValueOnce(new ApiRequestPreemptedError())
      .mockResolvedValueOnce('ready');
    await expect(retryPreemptedRead(succeedsAfterPreemption)).resolves.toBe('ready');
    expect(succeedsAfterPreemption).toHaveBeenCalledTimes(2);

    const retiredOwner = new ApiRequestOwnerRetiredError();
    const retired = vi.fn().mockRejectedValue(retiredOwner);
    await expect(retryPreemptedRead(retired)).rejects.toBe(retiredOwner);
    expect(retired).toHaveBeenCalledTimes(1);

    const offlineError = new Error('offline');
    const offline = vi.fn().mockRejectedValue(offlineError);
    await expect(retryPreemptedRead(offline)).rejects.toBe(offlineError);
    expect(offline).toHaveBeenCalledTimes(1);

    const bulk = vi.fn().mockRejectedValue(new BulkTransferPreemptedError());
    await expect(retryPreemptedRead(bulk)).rejects.toBeInstanceOf(BulkTransferPreemptedError);
    expect(bulk).toHaveBeenCalledTimes(1);

    const alwaysPreempted = vi.fn().mockRejectedValue(new ApiRequestPreemptedError());
    await expect(retryPreemptedRead(alwaysPreempted)).rejects.toBeInstanceOf(ApiRequestPreemptedError);
    expect(alwaysPreempted).toHaveBeenCalledTimes(3);
  });
});
