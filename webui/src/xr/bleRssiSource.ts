import { translate, type I18nLocale } from '../i18n';

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

export function describeBleRssiSupport(support: BleRssiSupport, locale: I18nLocale = 'en'): string {
  switch (support) {
    case 'supported': return translate(locale, 'ble.supported');
    case 'insecure_context': return translate(locale, 'ble.secure_context');
    case 'no_web_bluetooth': return translate(locale, 'ble.no_web_bluetooth');
    case 'no_le_scan': return translate(locale, 'ble.no_le_scan');
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
  private phase: 'idle' | 'starting' | 'active' | 'stopping' | 'disposed' = 'idle';
  private generation = 0;
  private pendingStart: Promise<boolean> | null = null;

  constructor(private readonly nav: NavigatorLike | undefined) {}

  get active(): boolean { return this.phase === 'active' && this.scan !== null; }
  get starting(): boolean { return this.phase === 'starting'; }

  /** Must be called from a user gesture. Resolves to false instead of throwing. */
  start(onAdvertisement: (advert: BleAdvertisement) => void): Promise<boolean> {
    const bluetooth = this.nav?.bluetooth;
    if (this.phase === 'disposed') return Promise.resolve(false);
    if (this.phase === 'active') return Promise.resolve(true);
    if (this.phase === 'starting' && this.pendingStart !== null) return this.pendingStart;
    if (bluetooth?.requestLEScan === undefined) return Promise.resolve(false);

    const generation = ++this.generation;
    const previous = this.pendingStart;
    this.phase = 'starting';
    let attempt: Promise<boolean>;
    const begin = async (): Promise<boolean> => {
      if (this.generation !== generation || this.phase !== 'starting') return false;
      return this.requestScan(bluetooth, generation, onAdvertisement);
    };
    attempt = previous === null ? begin() : previous.catch(() => false).then(begin);
    this.pendingStart = attempt;
    void attempt.finally(() => {
      if (this.pendingStart === attempt) this.pendingStart = null;
    }).catch(() => undefined);
    return attempt;
  }

  private async requestScan(
    bluetooth: BluetoothLike,
    generation: number,
    onAdvertisement: (advert: BleAdvertisement) => void,
  ): Promise<boolean> {
    if (bluetooth.requestLEScan === undefined) return false;
    const listener = (event: Event): void => {
      if (this.generation !== generation || this.phase !== 'active') return;
      const advert = parseRangingAdvertisement(event as unknown as AdvertisementLike);
      if (advert !== null) onAdvertisement(advert);
    };
    this.listener = listener;
    try {
      bluetooth.addEventListener('advertisementreceived', listener);
      const scan = await bluetooth.requestLEScan({ filters: [{ services: [RANGING_SERVICE_UUID] }], keepRepeatedDevices: true });
      if (this.generation !== generation || this.phase !== 'starting') {
        try { scan.stop(); } catch { /* obsolete scan */ }
        this.detachListener(listener);
        return false;
      }
      this.scan = scan;
      this.phase = 'active';
      return true;
    } catch {
      this.detachListener(listener);
      if (this.generation === generation && this.phase === 'starting') this.phase = 'idle';
      return false;
    }
  }

  stop(): void {
    if (this.phase === 'disposed') return;
    this.phase = 'stopping';
    this.generation += 1;
    try { this.scan?.stop(); } catch { /* already stopped */ }
    this.scan = null;
    this.detachListener(this.listener);
    this.phase = 'idle';
  }

  dispose(): void {
    if (this.phase === 'disposed') return;
    this.phase = 'stopping';
    this.generation += 1;
    try { this.scan?.stop(); } catch { /* already stopped */ }
    this.scan = null;
    this.detachListener(this.listener);
    this.phase = 'disposed';
  }

  private detachListener(listener: ((event: Event) => void) | null): void {
    if (listener === null || this.listener !== listener) return;
    this.nav?.bluetooth?.removeEventListener('advertisementreceived', listener);
    this.listener = null;
  }
}
