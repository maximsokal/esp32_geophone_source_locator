import { describe, expect, it } from 'vitest';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

import {
  RANGING_SERVICE_UUID, WebBluetoothRssiSource, bleRssiSupport, describeBleRssiSupport, parseRangingAdvertisement,
  type BleAdvertisement,
} from '../../src/xr/bleRssiSource';
import { BleRoughPlacementHud } from '../../src/components/BleRoughPlacementHud';
import type { BleNodeEstimate } from '../../src/xr/bleRoughPlacement';

/* MAX-146: RSSI hints are optional; the scanner never depends on them. */

function serviceData(bytes: number[]): { get(uuid: string): DataView | undefined } {
  const map = new Map([[RANGING_SERVICE_UUID, new DataView(Uint8Array.from(bytes).buffer)]]);
  return { get: (uuid: string) => map.get(uuid) };
}

class FakeBluetooth extends EventTarget {
  stopped = 0;
  options: unknown = null;
  constructor(private readonly fail = false) { super(); }
  requestLEScan = async (options: unknown) => {
    this.options = options;
    if (this.fail) throw new DOMException('denied', 'NotAllowedError');
    return { stop: () => { this.stopped += 1; } };
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
