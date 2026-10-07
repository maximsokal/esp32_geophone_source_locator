import { crc32 } from './crc32';

export interface ScanIdentity {
  readonly byteLength: number;
  readonly crc32: number;
  readonly frameId: string;
}

let wholeScanCrcInvocations = 0;
let wholeScanCrcBytesVisited = 0;
let persistenceBytesCopied = 0;

export function scanIdentityFromCrc(byteLength: number, checksum: number): ScanIdentity {
  wholeScanCrcInvocations += 1;
  wholeScanCrcBytesVisited += byteLength;
  return Object.freeze({
    byteLength,
    crc32: checksum >>> 0,
    frameId: `scan-crc32:${(checksum >>> 0).toString(16).padStart(8, '0')}`,
  });
}

export function createScanIdentity(bytes: Uint8Array): ScanIdentity {
  return scanIdentityFromCrc(bytes.byteLength, crc32(bytes));
}

export function recordPersistenceBytesCopied(byteLength: number): void {
  persistenceBytesCopied += byteLength;
}

export function getScanIdentityMetrics(): Readonly<{
  wholeScanCrcInvocations: number;
  wholeScanCrcBytesVisited: number;
  persistenceBytesCopied: number;
}> {
  return { wholeScanCrcInvocations, wholeScanCrcBytesVisited, persistenceBytesCopied };
}

export function resetScanIdentityMetrics(): void {
  wholeScanCrcInvocations = 0;
  wholeScanCrcBytesVisited = 0;
  persistenceBytesCopied = 0;
}
