import { afterEach, describe, expect, it, vi } from 'vitest';
import { createElement, isValidElement } from 'react';
import type { ReactElement, ReactNode } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

import {
  BulkTransferPreemptedError,
  saveObjectScanArtifact,
  sendControl,
} from '../../src/api/deviceApi';
import type { DeviceMainStatus, DevicePod, PodHeightReference } from '../../src/domain/device';
import SensorsFeature from '../../src/features/sensors/SensorsFeature';
import LevelingCard from '../../src/features/sensors/LevelingCard';

function pod(id: number, height_reference?: PodHeightReference): DevicePod {
  return { id, state: 'CONNECTED', height_reference } as DevicePod;
}

function findButton(node: ReactNode): ReactElement | null {
  if (Array.isArray(node)) {
    for (const child of node) {
      const button = findButton(child);
      if (button !== null) return button;
    }
    return null;
  }
  if (!isValidElement(node)) return null;
  const element = node as ReactElement<{ children?: ReactNode }>;
  if (element.type === 'button') return element;
  return findButton(element.props.children);
}

function renderCard(pods: DevicePod[], main?: DeviceMainStatus): string {
  return renderToStaticMarkup(createElement(LevelingCard, {
    gate: undefined, main, pods, onCalibrateHeights: async () => true,
  }));
}

afterEach(() => {
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

describe('height calibration UI (MAX-282)', () => {
  it('renders every reference state and relative Z only for valid references', () => {
    const html = renderCard([
      pod(1, { state: 'not_established', generation: 1, relative_z_mm: 80 }),
      pod(2, { state: 'valid', generation: 1, relative_z_mm: 12.34 }),
      pod(3, { state: 'rejected_moving', generation: 1, relative_z_mm: 81 }),
      pod(4, { state: 'sensor_invalid', generation: 1, relative_z_mm: 82 }),
    ]);

    expect(html).toContain('Not established');
    expect(html).toContain('Valid');
    expect(html).toContain('Rejected: node was moving');
    expect(html).toContain('Unavailable: sensor invalid');
    expect(html).toContain('12.3 mm');
    expect(html).not.toContain('80.0 mm');
    expect(html).not.toContain('81.0 mm');
    expect(html).not.toContain('82.0 mm');
  });

  it('reports differing generations and missing height data from older firmware', () => {
    const mismatch = renderCard([
      pod(1, { state: 'valid', generation: 3, relative_z_mm: 0 }),
      pod(2, { state: 'not_established', generation: 4, relative_z_mm: 0 }),
    ]);
    const olderFirmware = renderCard([pod(1)]);

    expect(mismatch).toContain('Height references have different generations.');
    expect(olderFirmware).toContain('Not reported by firmware');
    expect(olderFirmware).not.toContain('ESP API');
  });

  it('shows MAIN beside the REMOTEs and treats absent MAIN fields as older firmware', () => {
    const current = renderCard(
      [pod(1, { state: 'valid', generation: 6, relative_z_mm: -80 })],
      { height_reference: { state: 'valid', generation: 6, relative_z_mm: 0 } },
    );
    const unstable = renderCard([], {
      height_reference: { state: 'rejected_unstable', generation: 7, relative_z_mm: 0 },
    });
    const olderFirmware = renderCard([pod(1)]);

    expect(current).toContain('<td>MAIN</td>');
    expect(current).toContain('0.0 mm');
    expect(current).toContain('-80.0 mm');
    expect(unstable).toContain('Rejected: pressure was unstable');
    expect(olderFirmware).toContain('<td>MAIN</td>');
    expect(olderFirmware).toContain('Not reported by firmware');
  });

  it('routes height calibration through the critical lane', async () => {
    let signal!: AbortSignal;
    const starts: string[] = [];
    const fetchMock = vi.fn((url: string, init?: RequestInit) => {
      starts.push(url);
      if (url.includes('scan=save')) {
        signal = init?.signal as AbortSignal;
        return new Promise<Response>((_resolve, reject) => {
          signal.addEventListener('abort', () => reject(new Error('aborted')), { once: true });
        });
      }
      return Promise.resolve(new Response(JSON.stringify({ ok: true, action: 'calibrate_heights' }), { status: 200 }));
    });
    vi.stubGlobal('fetch', fetchMock);

    const bulk = saveObjectScanArtifact('OBJ_0001', 'S000001', new Uint8Array([1]));
    const calibration = sendControl('calibrate_heights');

    expect(signal.aborted).toBe(true);
    await expect(bulk).rejects.toBeInstanceOf(BulkTransferPreemptedError);
    await calibration;
    expect(starts).toEqual([
      '/api/control?scan=save&object_id=OBJ_0001&session_id=S000001',
      '/api/control',
    ]);
  });

  it('sends calibrate_heights when the button is pressed and shows API rejection text', async () => {
    const fetchMock = vi.fn(async (_url: string, init?: RequestInit) => new Response(
      JSON.stringify({ ok: true, action: 'calibrate_heights', requested_channels: [0, 1, 2, 3] }),
      { status: 200, headers: { 'Content-Type': 'application/json' } },
    ));
    vi.stubGlobal('fetch', fetchMock);
    let request: Promise<unknown> | null = null;
    const onCalibrateHeights = (): Promise<boolean> => {
      const sent = sendControl('calibrate_heights').then(() => true);
      request = sent;
      return sent;
    };
    const button = findButton(LevelingCard({ gate: undefined, pods: [], onCalibrateHeights }));
    expect(button).not.toBeNull();
    (button?.props as { onClick?: () => void }).onClick?.();
    await request;

    expect(fetchMock.mock.calls[0][0]).toBe('/api/control');
    expect(JSON.parse(fetchMock.mock.calls[0][1]?.body as string)).toEqual({ action: 'calibrate_heights' });

    fetchMock.mockResolvedValueOnce(new Response(JSON.stringify({ ok: false, error: 'sensors are moving' }), {
      status: 409, statusText: 'Conflict', headers: { 'Content-Type': 'application/json' },
    }));
    let error = '';
    try {
      await sendControl('calibrate_heights');
    } catch (reason) {
      error = reason instanceof Error ? reason.message : String(reason);
    }
    const html = renderToStaticMarkup(createElement(SensorsFeature, {
      status: null,
      object: null,
      error,
      onCalibrateHeights: async () => false,
    }));

    expect(error).toContain('ESP API 409 POST /api/control: sensors are moving');
    expect(html).toContain('ESP API: ESP API 409 POST /api/control: sensors are moving');
  });
});
