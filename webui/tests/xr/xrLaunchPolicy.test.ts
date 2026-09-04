import { describe, expect, it, vi } from 'vitest';

import {
  canAttemptImmersiveAr,
  launchImmersiveArFromUserGesture,
} from '../../src/xr/xrLaunchPolicy';

describe('canAttemptImmersiveAr', () => {
  it('keeps launch available while the advisory capability probe is pending', () => {
    expect(canAttemptImmersiveAr(null)).toBe(true);
  });

  it('allows a known supported immersive AR environment', () => {
    expect(canAttemptImmersiveAr({
      secureContext: true,
      webGl: true,
      xrApi: true,
      immersiveAr: true,
    })).toBe(true);
  });

  it.each(['secureContext', 'webGl', 'xrApi', 'immersiveAr'] as const)(
    'blocks launch after %s is conclusively reported unavailable',
    (key) => {
      const capabilities = {
        secureContext: true,
        webGl: true,
        xrApi: true,
        immersiveAr: true,
      };
      capabilities[key] = false;
      expect(canAttemptImmersiveAr(capabilities)).toBe(false);
    },
  );
});

describe('launchImmersiveArFromUserGesture', () => {
  it('starts WebXR before awaiting compute preparation', async () => {
    const order: string[] = [];
    let resolvePreparation!: (value: { workers: number }) => void;
    const preparation = new Promise<{ workers: number }>((resolve) => {
      resolvePreparation = resolve;
    });

    const launch = launchImmersiveArFromUserGesture({
      beginScan: () => order.push('begin'),
      startRuntime: async () => {
        order.push('request-session');
      },
      prepareCompute: () => {
        order.push('prepare-compute');
        return preparation;
      },
      onPrepared: (prepared) => order.push(`prepared-${prepared.workers}`),
    });

    expect(order).toEqual(['begin', 'request-session', 'prepare-compute']);

    resolvePreparation({ workers: 2 });
    await expect(launch).resolves.toEqual({ workers: 2 });
    expect(order).toEqual([
      'begin',
      'request-session',
      'prepare-compute',
      'prepared-2',
    ]);
  });

  it('propagates runtime launch rejection', async () => {
    const error = new Error('XR denied');
    const onPrepared = vi.fn();

    await expect(launchImmersiveArFromUserGesture({
      beginScan: vi.fn(),
      startRuntime: async () => { throw error; },
      prepareCompute: async () => ({ workers: 1 }),
      onPrepared,
    })).rejects.toBe(error);

    expect(onPrepared).toHaveBeenCalledWith({ workers: 1 });
  });
});
