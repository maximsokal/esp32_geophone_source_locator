/**
 * MAX-146: optional BLE RSSI source for pre-anchor rough placement.
 *
 * The nRF54L15 nodes advertise the Bluetooth Ranging Service (0x185B) with a
 * service-data byte carrying their CS node id -- but only while they wait
 * for a Channel Sounding connection, so packets arrive in bursts.
 *
 * Web Bluetooth LE scanning (`navigator.bluetooth.requestLEScan`) is
 * experimental and missing on most phones/browsers. The scanner must work
 * without it: support is detected up front and every failure degrades to
 * "RSSI hints unavailable", never to a scanner error.
 */

export const RANGING_SERVICE_UUID = '0000185b-0000-1000-8000-00805f9b34fb';

export type BleRssiSupport = 'supported' | 'insecure_context' | 'no_web_bluetooth' | 'no_le_scan';

export interface BleAdvertisement {
  nodeId: number;
  rssiDbm: number;
  txPowerDbm: number | null;
}

interface AdvertisementLike {
  rssi?: number | null;
  txPower?: number | null;
  serviceData?: { get(uuid: string): DataView | undefined } | null;
}

interface LeScanLike { active?: boolean; stop(): void }

interface BluetoothLike extends EventTarget {
  requestLEScan?: (options: unknown) => Promise<LeScanLike>;
}

interface NavigatorLike { bluetooth?: BluetoothLike }

export function bleRssiSupport(nav: NavigatorLike | undefined, secureContext: boolean): BleRssiSupport {
  if (!secureContext) return 'insecure_context';
  if (nav?.bluetooth === undefined) return 'no_web_bluetooth';
  if (typeof nav.bluetooth.requestLEScan !== 'function') return 'no_le_scan';
  return 'supported';
}

export function describeBleRssiSupport(support: BleRssiSupport): string {
  switch (support) {
    case 'supported': return 'BLE RSSI hints available (rough placement only).';
    case 'insecure_context': return 'BLE RSSI hints need a secure (HTTPS) page.';
    case 'no_web_bluetooth': return 'This browser has no Web Bluetooth; scanning works without RSSI hints.';
    case 'no_le_scan': return 'This browser cannot passively scan BLE advertisements; scanning works without RSSI hints.';
  }
}

/** Returns null for anything that is not a node's Ranging Service advert. */
export function parseRangingAdvertisement(event: AdvertisementLike): BleAdvertisement | null {
  const rssi = event.rssi;
  if (typeof rssi !== 'number' || !Number.isFinite(rssi)) return null;
  const data = event.serviceData?.get(RANGING_SERVICE_UUID);
  if (data === undefined || data.byteLength < 1) return null;
  const nodeId = data.getUint8(0);
  if (nodeId === 0xff) return null; /* not yet assigned by the bootstrap */
  const tx = event.txPower;
  return { nodeId, rssiDbm: rssi, txPowerDbm: typeof tx === 'number' && Number.isFinite(tx) ? tx : null };
}

export class WebBluetoothRssiSource {
  private scan: LeScanLike | null = null;
  private listener: ((event: Event) => void) | null = null;

  constructor(private readonly nav: NavigatorLike | undefined) {}

  get active(): boolean { return this.scan !== null; }

  /** Must be called from a user gesture. Resolves to false instead of throwing. */
  async start(onAdvertisement: (advert: BleAdvertisement) => void): Promise<boolean> {
    const bluetooth = this.nav?.bluetooth;
    if (this.scan !== null) return true;
    if (bluetooth?.requestLEScan === undefined) return false;
    const listener = (event: Event): void => {
      const advert = parseRangingAdvertisement(event as unknown as AdvertisementLike);
      if (advert !== null) onAdvertisement(advert);
    };
    try {
      bluetooth.addEventListener('advertisementreceived', listener);
      this.listener = listener;
      this.scan = await bluetooth.requestLEScan({ filters: [{ services: [RANGING_SERVICE_UUID] }], keepRepeatedDevices: true });
      return true;
    } catch {
      this.stop();
      return false;
    }
  }

  stop(): void {
    try { this.scan?.stop(); } catch { /* already stopped */ }
    this.scan = null;
    if (this.listener !== null) this.nav?.bluetooth?.removeEventListener('advertisementreceived', this.listener);
    this.listener = null;
  }
}
