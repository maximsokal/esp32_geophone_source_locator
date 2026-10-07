import { describe, expect, it } from 'vitest';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

import {
  RANGING_SERVICE_UUID, WebBluetoothRssiSource, bleRssiSupport, describeBleRssiSupport, parseRangingAdvertisement,
  type BleAdvertisement,
} from '../../src/xr/bleRssiSource';
import { BleRoughPlacementHud } from '../../src/components/BleRoughPlacementHud';
import type { BleNodeEstimate } from '../../src/xr/bleRoughPlacement';
import { deferred, type Deferred } from '../xr/support/xrFakes';

/* MAX-146: RSSI hints are optional; the scanner never depends on them. */

function serviceData(bytes: number[]): { get(uuid: string): DataView | undefined } {
  const map = new Map([[RANGING_SERVICE_UUID, new DataView(Uint8Array.from(bytes).buffer)]]);
  return { get: (uuid: string) => map.get(uuid) };
}

interface FakeScan {
  stop(): void;
  stopCalls: number;
}

class FakeBluetooth {
  stopped = 0;
  requestCalls = 0;
  liveScans = 0;
  listenerRemovals = 0;
  options: unknown = null;
  private readonly listeners = new Map<string, Set<EventListenerOrEventListenerObject>>();
  private readonly queuedScans: Promise<FakeScan>[] = [];

  constructor(private readonly fail = false) {}
  addEventListener(type: string, listener: EventListenerOrEventListenerObject | null): void {
    if (listener === null) return;
    const bucket = this.listeners.get(type) ?? new Set<EventListenerOrEventListenerObject>();
    bucket.add(listener);
    this.listeners.set(type, bucket);
  }

  removeEventListener(type: string, listener: EventListenerOrEventListenerObject | null): void {
    if (listener === null) return;
    this.listenerRemovals += 1;
    this.listeners.get(type)?.delete(listener);
  }

  dispatchEvent(event: Event): boolean {
    for (const listener of this.listeners.get(event.type) ?? []) {
      if (typeof listener === 'function') listener.call(this, event);
      else listener.handleEvent(event);
    }
    return true;
  }

  listenerCount(type = 'advertisementreceived'): number {
    return this.listeners.get(type)?.size ?? 0;
  }

  deferNextScan(): Deferred<FakeScan> {
    const pending = deferred<FakeScan>();
    this.queuedScans.push(pending.promise);
    return pending;
  }

  createScan(): FakeScan {
    this.liveScans += 1;
    let stopped = false;
    const scan: FakeScan = {
      stopCalls: 0,
      stop: () => {
        if (stopped) return;
        stopped = true;
        scan.stopCalls += 1;
        this.stopped += 1;
        this.liveScans -= 1;
      },
    };
    return scan;
  }

  requestLEScan = async (options: unknown): Promise<FakeScan> => {
    this.requestCalls += 1;
    this.options = options;
    if (this.fail) throw new DOMException('denied', 'NotAllowedError');
    const queued = this.queuedScans.shift();
    return queued === undefined ? this.createScan() : queued;
  };

  advertise(fields: Record<string, unknown>): void {
    this.dispatchEvent(Object.assign(new Event('advertisementreceived'), fields));
  }
}

describe('bleRssiSupport', () => {
  it('degrades to a described "unavailable" state instead of failing', () => {
    expect(bleRssiSupport({ bluetooth: new FakeBluetooth() }, false)).toBe('insecure_context');
    expect(bleRssiSupport({}, true)).toBe('no_web_bluetooth');
    expect(bleRssiSupport(undefined, true)).toBe('no_web_bluetooth');
    expect(bleRssiSupport({ bluetooth: new EventTarget() }, true)).toBe('no_le_scan');
    expect(bleRssiSupport({ bluetooth: new FakeBluetooth() }, true)).toBe('supported');
    expect(describeBleRssiSupport('no_le_scan')).toContain('works without RSSI hints');
  });
});

describe('parseRangingAdvertisement', () => {
  it('reads the node id from the Ranging Service data', () => {
    expect(parseRangingAdvertisement({ rssi: -71, txPower: 0, serviceData: serviceData([3]) }))
      .toEqual({ nodeId: 3, rssiDbm: -71, txPowerDbm: 0 });
    expect(parseRangingAdvertisement({ rssi: -71, txPower: null, serviceData: serviceData([0]) })!.txPowerDbm).toBeNull();
  });

  it('ignores foreign, unassigned or RSSI-less adverts', () => {
    expect(parseRangingAdvertisement({ rssi: -71, serviceData: serviceData([0xff]) })).toBeNull();
    expect(parseRangingAdvertisement({ rssi: -71, serviceData: serviceData([]) })).toBeNull();
    expect(parseRangingAdvertisement({ rssi: null, serviceData: serviceData([1]) })).toBeNull();
    expect(parseRangingAdvertisement({ rssi: -71, serviceData: { get: () => undefined } })).toBeNull();
    expect(parseRangingAdvertisement({ rssi: -71 })).toBeNull();
  });
});

describe('WebBluetoothRssiSource', () => {
  it('scans only for the Ranging Service and stops cleanly', async () => {
    const bluetooth = new FakeBluetooth();
    const source = new WebBluetoothRssiSource({ bluetooth });
    const received: BleAdvertisement[] = [];
    expect(await source.start((a) => received.push(a))).toBe(true);
    expect(source.active).toBe(true);
    expect(bluetooth.options).toEqual({ filters: [{ services: [RANGING_SERVICE_UUID] }], keepRepeatedDevices: true });
    bluetooth.advertise({ rssi: -66, txPower: null, serviceData: serviceData([1]) });
    bluetooth.advertise({ rssi: -66, serviceData: serviceData([0xff]) });
    expect(received).toEqual([{ nodeId: 1, rssiDbm: -66, txPowerDbm: null }]);
    source.stop();
    expect(bluetooth.stopped).toBe(1);
    expect(source.active).toBe(false);
    bluetooth.advertise({ rssi: -60, serviceData: serviceData([2]) });
    expect(received).toHaveLength(1);
  });

  it('reports false (not an exception) when the browser refuses or lacks scanning', async () => {
    const bluetooth = new FakeBluetooth(true);
    const source = new WebBluetoothRssiSource({ bluetooth });
    const received: BleAdvertisement[] = [];
    expect(await source.start((a) => received.push(a))).toBe(false);
    expect(source.active).toBe(false);
    bluetooth.advertise({ rssi: -60, serviceData: serviceData([2]) });
    expect(received).toHaveLength(0);
    expect(await new WebBluetoothRssiSource({}).start(() => undefined)).toBe(false);
    expect(await new WebBluetoothRssiSource(undefined).start(() => undefined)).toBe(false);
  });

  it('stops a handle that resolves after stop and removes its advertisement listener', async () => {
    const bluetooth = new FakeBluetooth();
    const pending = bluetooth.deferNextScan();
    const source = new WebBluetoothRssiSource({ bluetooth });
    let received = 0;
    const starting = source.start(() => { received += 1; });

    expect(bluetooth.requestCalls).toBe(1);
    expect(bluetooth.listenerCount()).toBe(1);
    source.stop();
    expect(bluetooth.listenerCount()).toBe(0);
    const late = bluetooth.createScan();
    pending.resolve(late);

    await expect(starting).resolves.toBe(false);
    expect(late.stopCalls).toBe(1);
    expect(source.active).toBe(false);
    expect(bluetooth.liveScans).toBe(0);
    expect(bluetooth.listenerRemovals).toBe(1);
    bluetooth.advertise({ rssi: -60, serviceData: serviceData([1]) });
    expect(received).toBe(0);
  });

  it('shares the same in-flight request between concurrent starts', async () => {
    const bluetooth = new FakeBluetooth();
    const pending = bluetooth.deferNextScan();
    const source = new WebBluetoothRssiSource({ bluetooth });
    const first = source.start(() => undefined);
    const second = source.start(() => undefined);

    expect(second).toBe(first);
    expect(bluetooth.requestCalls).toBe(1);
    const scan = bluetooth.createScan();
    pending.resolve(scan);
    await expect(Promise.all([first, second])).resolves.toEqual([true, true]);
    expect(source.active).toBe(true);
    source.stop();
    expect(scan.stopCalls).toBe(1);
    expect(bluetooth.listenerCount()).toBe(0);
  });

  it('queues a fresh start behind a stopped request and ignores the late first scan', async () => {
    const bluetooth = new FakeBluetooth();
    const firstPending = bluetooth.deferNextScan();
    const secondPending = bluetooth.deferNextScan();
    const source = new WebBluetoothRssiSource({ bluetooth });
    let firstReceived = 0;
    let secondReceived = 0;
    const first = source.start(() => { firstReceived += 1; });
    source.stop();
    const second = source.start(() => { secondReceived += 1; });

    expect(bluetooth.requestCalls).toBe(1);
    const firstScan = bluetooth.createScan();
    firstPending.resolve(firstScan);
    await expect(first).resolves.toBe(false);
    for (let index = 0; index < 5 && bluetooth.requestCalls < 2; index += 1) await Promise.resolve();
    expect(bluetooth.requestCalls).toBe(2);
    const secondScan = bluetooth.createScan();
    secondPending.resolve(secondScan);
    await expect(second).resolves.toBe(true);
    bluetooth.advertise({ rssi: -60, serviceData: serviceData([2]) });
    expect(firstReceived).toBe(0);
    expect(secondReceived).toBe(1);
    expect(firstScan.stopCalls).toBe(1);
    source.stop();
    expect(secondScan.stopCalls).toBe(1);
    expect(bluetooth.liveScans).toBe(0);
    expect(bluetooth.listenerCount()).toBe(0);
  });

  it('returns handles and listeners to baseline through 100 delayed unmount teardowns', async () => {
    const bluetooth = new FakeBluetooth();
    for (let index = 0; index < 100; index += 1) {
      const pending = bluetooth.deferNextScan();
      const source = new WebBluetoothRssiSource({ bluetooth });
      let received = 0;
      const starting = source.start(() => { received += 1; });
      source.stop();
      const late = bluetooth.createScan();
      pending.resolve(late);

      await expect(starting).resolves.toBe(false);
      bluetooth.advertise({ rssi: -60, serviceData: serviceData([1]) });
      expect(late.stopCalls).toBe(1);
      expect(bluetooth.liveScans).toBe(0);
      expect(bluetooth.listenerCount()).toBe(0);
      expect(bluetooth.listenerRemovals).toBe(index + 1);
      expect(received).toBe(0);
    }
  });
});

describe('BleRoughPlacementHud', () => {
  const base: BleNodeEstimate = {
    nodeId: 0, label: 'MAIN', state: 'not_detected', sampleCount: 0, lastSeenMs: null,
    confidence: 0, centre: null, ringM: null, uncertaintyM: null,
  };
  const pose = { position: [0, 1.4, 0] as const, orientation: [0, 0, 0, 1] as const };

  it('shows ranges and uncertainty, not precise coordinates, plus off-screen arrows', () => {
    const html = renderToStaticMarkup(createElement(BleRoughPlacementHud, {
      pose,
      estimates: [
        base,
        { ...base, nodeId: 1, label: 'REMOTE 1', state: 'ble_detected', sampleCount: 4, confidence: 0.1, centre: [0, 1.4, 0], ringM: { medianM: 4, minM: 2.1, maxM: 9.6 } },
        { ...base, nodeId: 2, label: 'REMOTE 2', state: 'rough', sampleCount: 20, confidence: 0.2, centre: [6, 1.4, 0], uncertaintyM: 3.2 },
        { ...base, nodeId: 3, label: 'REMOTE 3', state: 'rough', sampleCount: 20, confidence: 0.2, centre: [0, 1.4, -6], uncertaintyM: 2 },
        { ...base, nodeId: 4, label: 'REMOTE 4', state: 'anchored', confidence: 1, centre: [-3, 0, 0] },
      ],
    }));
    expect(html).toContain('<strong>MAIN</strong> not heard');
    expect(html).toContain('~2–10 m · walk to localize');
    expect(html).toContain('rough ±3.2 m');
    expect(html).toContain('visual anchor ✓');
    expect(html).toContain('never replace Channel Sounding');
    /* REMOTE 2 is to the right and REMOTE 4 to the left: arrows. REMOTE 3 is ahead: none. */
    expect(html).toContain('aria-label="REMOTE 2 off screen, about 6 m"');
    expect(html).toContain('aria-label="REMOTE 4 off screen, about 3 m"');
    expect(html).not.toContain('REMOTE 3 off screen');
    /* The ring-only node never gets an arrow (it has no direction). */
    expect(html).not.toContain('REMOTE 1 off screen');
  });

  it('draws no arrows without a pose', () => {
    const html = renderToStaticMarkup(createElement(BleRoughPlacementHud, {
      pose: null, estimates: [{ ...base, state: 'rough', centre: [6, 0, 0], uncertaintyM: 2 }],
    }));
    expect(html).not.toContain('off screen');
  });
});
