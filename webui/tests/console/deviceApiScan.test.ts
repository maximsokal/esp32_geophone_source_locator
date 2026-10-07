import { afterEach, describe, expect, it, vi } from 'vitest';

import {
  ApiRequestPreemptedError,
  fetchDeviceStatus,
  fetchScanInfo,
  loadObjectScanArtifact,
  ScanStorageBlockedRecordingError,
  saveObjectScanArtifact,
  saveSessionOccupancyArtifact,
  saveSessionPreviewArtifact,
  saveSessionRegistration,
  sendControl,
} from '../../src/api/deviceApi';

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { 'Content-Type': 'application/json' },
  });
}

function validDeviceStatus() {
  return {
    system: { state: 'NOT_READY', mode: 'NORMAL', uptime_ms: 0, acquisition_ready: false, ready_pods: 0, required_pods: 0 },
    power: { valid: false, voltage_v: 0, current_a: 0, power_w: 0 },
    environment: { valid: false, temperature_c: 0, humidity_percent: 0, pressure_pa: 0 },
    mux: { valid: false, active_channel: -1, active_pod: -1 },
    pods: [],
    acquisition: { running: false, requested_running: false, hardware_ready: false, source: 'NONE', sample_rate_hz: 0, samples_recorded: 0, recording_ms: 0 },
  };
}

afterEach(() => {
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

describe('session-owned 3D scan transport (MAX-167)', () => {
  it('uploads the artifact as exact opaque bytes to the selected recording/session', async () => {
    const artifact = new Uint8Array([0x47, 0x58, 0x52, 0x53, 1, 2, 3, 255]);
    const fetchMock = vi.fn(async (_url: string, init?: RequestInit) => {
      const body = init?.body;
      expect(body).toBeInstanceOf(ArrayBuffer);
      expect(Array.from(new Uint8Array(body as ArrayBuffer))).toEqual(Array.from(artifact));
      return jsonResponse({ ok: true, action: 'scan_save', object_id: 'OBJ_0008', bytes: artifact.length, id: 'scan-crc32:12345678' });
    });
    vi.stubGlobal('fetch', fetchMock);

    const result = await saveObjectScanArtifact('OBJ_0008', 'S000001', artifact);

    expect(result.bytes).toBe(artifact.length);
    expect(result.id).toBe('scan-crc32:12345678');
    expect(fetchMock).toHaveBeenCalledTimes(1);
    expect(fetchMock.mock.calls[0][0]).toBe('/api/control?scan=save&object_id=OBJ_0008&session_id=S000001');
    const init = fetchMock.mock.calls[0][1] as RequestInit;
    expect(init.method).toBe('POST');
    expect((init.headers as Record<string, string>)['Content-Type']).toBe('application/octet-stream');
  });

  it('builds staging URLs for scan, occupancy, preview, and registration uploads', async () => {
    const fetchMock = vi.fn(async (_url: string, _init?: RequestInit) => jsonResponse({ ok: true, bytes: 1 }));
    vi.stubGlobal('fetch', fetchMock);

    await saveObjectScanArtifact('OBJ_0008', 'staging', new Uint8Array([1]));
    await saveSessionOccupancyArtifact('OBJ_0008', 'staging', new Uint8Array([2]), 'scan-crc32:01234567');
    await saveSessionPreviewArtifact('OBJ_0008', 'staging', new Uint8Array([3]), 'scan-crc32:01234567');
    await saveSessionRegistration('OBJ_0008', 'staging', '{}');

    expect(fetchMock.mock.calls.map(([url]) => url)).toEqual([
      '/api/control?scan=save&object_id=OBJ_0008&session_id=staging',
      '/api/control?scan=save&object_id=OBJ_0008&session_id=staging&kind=occupancy&scan_id=scan-crc32%3A01234567',
      '/api/control?scan=save&object_id=OBJ_0008&session_id=staging&kind=preview&scan_id=scan-crc32%3A01234567',
      '/api/control?scan=save&object_id=OBJ_0008&session_id=staging&kind=registration',
    ]);
  });

  it('parses scan and derivative generation identities while tolerating legacy info responses', async () => {
    const responses = [
      jsonResponse({ ok: true, bytes: 12, id: 'scan-crc32:12345678' }),
      jsonResponse({ ok: true, bytes: 5, source_id: 'scan-crc32:12345678' }),
      jsonResponse({ ok: true, bytes: 12 }),
    ];
    vi.stubGlobal('fetch', vi.fn(async () => responses.shift() as Response));

    await expect(fetchScanInfo('OBJ_0008', 'S000001', 'scan')).resolves.toEqual({
      present: true, bytes: 12, id: 'scan-crc32:12345678', sourceId: null,
    });
    await expect(fetchScanInfo('OBJ_0008', 'S000001', 'occupancy')).resolves.toEqual({
      present: true, bytes: 5, id: null, sourceId: 'scan-crc32:12345678',
    });
    await expect(fetchScanInfo('OBJ_0008', 'S000001', 'scan')).resolves.toEqual({
      present: true, bytes: 12, id: null, sourceId: null,
    });
  });

  it('round-trips loaded artifact bytes without JSON conversion', async () => {
    const artifact = new Uint8Array([1, 9, 3, 7, 5, 255]);
    const fetchMock = vi.fn(async () => new Response(artifact.slice().buffer, {
      status: 200,
      headers: { 'Content-Type': 'application/octet-stream' },
    }));
    vi.stubGlobal('fetch', fetchMock);

    const loaded = await loadObjectScanArtifact('OBJ_0008', 'S000001');

    expect(loaded).not.toBeNull();
    expect(Array.from(loaded ?? [])).toEqual(Array.from(artifact));
    expect(fetchMock.mock.calls[0][0]).toBe('/api/control?scan=load&object_id=OBJ_0008&session_id=S000001');
  });

  it('treats only HTTP 404 as no saved scan', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => jsonResponse({ error: 'ESP_ERR_NOT_FOUND' }, 404)));
    await expect(loadObjectScanArtifact('OBJ_0010', 'S000002')).resolves.toBeNull();
  });

  it('returns a distinct blocked_recording outcome for scan-save 409 responses', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => jsonResponse({
      ok: false, action: 'scan_save', busy_reason: 'recording_active',
    }, 409)));

    await expect(saveObjectScanArtifact('OBJ_0008', 'S000003', new Uint8Array([1, 2, 3]))).resolves.toMatchObject({
      ok: false, outcome: 'blocked_recording', busy_reason: 'recording_active',
    });
  });

  it('distinguishes definite scan rejection from an ambiguous transport failure', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => jsonResponse({ ok: false, error: 'ESP_ERR_INVALID_SIZE' }, 400)));
    await expect(saveObjectScanArtifact('OBJ_0008', 'S000021', new Uint8Array([1, 2, 3])))
      .rejects.toMatchObject({ outcome: 'confirmed_not_committed', httpStatus: 400 });

    vi.stubGlobal('fetch', vi.fn(async () => { throw new Error('socket reset'); }));
    await expect(saveObjectScanArtifact('OBJ_0008', 'S000021', new Uint8Array([1, 2, 3])))
      .rejects.toMatchObject({ outcome: 'unknown' });
  });

  it('lets critical control preempt scan-info reads', async () => {
    let infoSignal!: AbortSignal;
    let releaseInfo!: (response: Response) => void;
    let signalInfoStarted!: () => void;
    let signalControlStarted!: () => void;
    const infoStarted = new Promise<void>((resolve) => { signalInfoStarted = resolve; });
    const controlStarted = new Promise<void>((resolve) => { signalControlStarted = resolve; });
    const fetchMock = vi.fn((url: string, init?: RequestInit) => {
      if (url.includes('scan=info')) {
        infoSignal = init?.signal as AbortSignal;
        signalInfoStarted();
        return new Promise<Response>((resolve, reject) => {
          releaseInfo = resolve;
          infoSignal.addEventListener('abort', () => reject(new Error('aborted')), { once: true });
        });
      }
      signalControlStarted();
      return Promise.resolve(jsonResponse({ ok: true, action: 'measurement_stop' }));
    });
    vi.stubGlobal('fetch', fetchMock);

    const infoOutcome = fetchScanInfo('OBJ_0008', 'S000022').then(
      () => 'resolved',
      (reason: unknown) => reason,
    );
    await infoStarted;
    const stop = sendControl('measurement_stop');
    let timeoutId: ReturnType<typeof setTimeout> | undefined;
    const stopStartedBeforeInfoFinished = await Promise.race([
      controlStarted.then(() => true),
      new Promise<boolean>((resolve) => { timeoutId = globalThis.setTimeout(() => resolve(false), 50); }),
    ]);
    if (timeoutId !== undefined) globalThis.clearTimeout(timeoutId);
    if (!infoSignal.aborted) releaseInfo(jsonResponse({ ok: true, bytes: 1, id: 'scan-crc32:00000001' }));
    await stop;

    expect(stopStartedBeforeInfoFinished).toBe(true);
    expect(infoSignal.aborted).toBe(true);
    expect(await infoOutcome).toBeInstanceOf(ApiRequestPreemptedError);
  });

  it('classifies a scan-load 409 as blocked_recording', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => jsonResponse({
      ok: false, action: 'scan_load', busy_reason: 'recording_active',
    }, 409)));

    await expect(loadObjectScanArtifact('OBJ_0008', 'S000004')).rejects.toBeInstanceOf(ScanStorageBlockedRecordingError);
  });

  it('keeps a status poll queued behind bulk scan upload without aborting it', async () => {
    let resolveSave!: (response: Response) => void;
    let saveSignal!: AbortSignal;
    const fetchMock = vi.fn((url: string, init?: RequestInit) => {
      if (url.includes('scan=save')) {
        saveSignal = init?.signal as AbortSignal;
        return new Promise<Response>((resolve) => { resolveSave = resolve; });
      }
      return Promise.resolve(jsonResponse(validDeviceStatus()));
    });
    vi.stubGlobal('fetch', fetchMock);

    const save = saveObjectScanArtifact('OBJ_0008', 'S000001', new Uint8Array([1, 2, 3]));
    const status = fetchDeviceStatus(false);

    expect(fetchMock).toHaveBeenCalledTimes(1);
    expect(saveSignal.aborted).toBe(false);
    resolveSave(jsonResponse({ ok: true, action: 'scan_save', object_id: 'OBJ_0008', bytes: 3 }));
    await save;
    await status;

    expect(fetchMock).toHaveBeenCalledTimes(2);
    expect(fetchMock.mock.calls[1][0]).toBe('/api/status');
    expect(saveSignal.aborted).toBe(false);
  });
});
