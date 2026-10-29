import { afterEach, describe, expect, it, vi } from 'vitest';

import { sendControl, sendWorkflowCommand } from '../../src/api/deviceApi';
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


describe('unconfirmed control outcomes', () => {
  it.each([
    { ok: 'true', action: 'measurement_start' },
    { ok: true, action: 'measurement_stop' },
    { ok: true, action: 'measurement_start', pending: 'yes' },
    { ok: true, action: 'measurement_start', request_id: -1 },
    { ok: true, action: 'measurement_start', accepted: 'yes' },
    { ok: true, action: 'measurement_start', accepted: false },
    { ok: true, action: 'measurement_start', pending: true, completed: true },
  ])('does not project a malformed successful control response: %j', async (body) => {
    vi.stubGlobal('fetch', vi.fn(async () => Response.json(body)));
    const project = vi.fn();
    await expect(runAfterControlConfirmation(sendControl('measurement_start'), project))
      .rejects.toMatchObject({ name: 'ControlOutcomeUnknownError', outcome: 'unknown' });
    expect(project).not.toHaveBeenCalled();
  });

  it('preserves a definite recording interlock rejection', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => Response.json({
      ok: false, action: 'measurement_start', busy_reason: 'recording_active',
    }, { status: 409 })));
    await expect(sendControl('measurement_start')).rejects.toMatchObject({ name: 'ScanStorageBlockedRecordingError' });
  });

  it('reports a lost response after execution without replaying the POST', async () => {
    let executions = 0;
    const fetchMock = vi.fn(async () => {
      ++executions;
      return new Response('{"ok":tru');
    });
    vi.stubGlobal('fetch', fetchMock);
    await expect(sendControl('measurement_start')).rejects.toMatchObject({ outcome: 'unknown' });
    expect(executions).toBe(1);
    expect(fetchMock).toHaveBeenCalledOnce();
  });

  it('reports a dropped workflow reply as unknown and never retries the write', async () => {
    const fetchMock = vi.fn(async () => { throw new Error('response lost after commit'); });
    vi.stubGlobal('fetch', fetchMock);
    expect(await sendWorkflowCommand('START_MEASUREMENT', 7)).toMatchObject({ ok: false, reason: 'unknown_outcome' });
    expect(fetchMock).toHaveBeenCalledOnce();
  });

  it('does not accept an unvalidated workflow record as command success', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => Response.json({ ok: true, workflow: { step: 'RECORDING', generation: '7' } })));
    expect(await sendWorkflowCommand('START_RECORDING', 7)).toMatchObject({ ok: false, reason: 'unknown_outcome' });
  });
});
