import { afterEach, describe, expect, it, vi } from 'vitest';
import { clientDiagnostics } from '../../src/data/clientDiagnostics';

afterEach(() => {
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

describe('client diagnostics runtime isolation', () => {
  it('treats explicit flush as a no-op when no browser window exists', async () => {
    const fetchMock = vi.fn();
    vi.stubGlobal('fetch', fetchMock);

    clientDiagnostics.info('NODE_TEST_DIAGNOSTIC');

    await expect(clientDiagnostics.flush()).resolves.toBeUndefined();
    expect(fetchMock).not.toHaveBeenCalled();
  });
});
