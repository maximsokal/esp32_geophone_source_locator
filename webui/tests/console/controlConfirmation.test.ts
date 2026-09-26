import { afterEach, describe, expect, it, vi } from 'vitest';

import { sendControl } from '../../src/api/deviceApi';
import { runAfterControlConfirmation } from '../../src/features/objects/controlConfirmation';

afterEach(() => vi.unstubAllGlobals());

describe('control confirmation boundary', () => {
  it('shows acquisition failure from HTTP 503 and skips projection and transition polling', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => Response.json({
      ok: false,
      action: 'measurement_start',
      error_class: 'acquisition_unavailable',
      error: 'ESP_ERR_INVALID_STATE',
    }, { status: 503 })));
    const project = vi.fn();
    const poll = vi.fn();

    await expect(runAfterControlConfirmation(sendControl('measurement_start', { object_id: 'OBJ_0001' }), () => {
      project();
      poll();
    })).rejects.toThrow('Acquisition unavailable: ESP_ERR_INVALID_STATE');
    expect(project).not.toHaveBeenCalled();
    expect(poll).not.toHaveBeenCalled();
  });

  it('treats HTTP 200 with ok=false as a failure and skips projection and polling', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => Response.json({
      ok: false,
      action: 'measurement_start',
      error_class: 'acquisition_unavailable',
      error: 'ESP_ERR_INVALID_STATE',
    })));
    const project = vi.fn();
    const poll = vi.fn();

    await expect(runAfterControlConfirmation(sendControl('measurement_start', { object_id: 'OBJ_0001' }), () => {
      project();
      poll();
    })).rejects.toThrow('Acquisition unavailable: ESP_ERR_INVALID_STATE');
    expect(project).not.toHaveBeenCalled();
    expect(poll).not.toHaveBeenCalled();
  });

  it('runs projection and transition polling after an explicit success', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => Response.json({ ok: true, action: 'measurement_start' })));
    const project = vi.fn();
    const poll = vi.fn(async () => undefined);

    await expect(runAfterControlConfirmation(sendControl('measurement_start', { object_id: 'OBJ_0001' }), async () => {
      project();
      await poll();
      return true;
    })).resolves.toBe(true);
    expect(project).toHaveBeenCalledOnce();
    expect(poll).toHaveBeenCalledOnce();
  });
});
