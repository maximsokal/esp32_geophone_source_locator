import { crc32 } from './crc32';

export interface ScanIdentity {
  readonly byteLength: number;
  readonly crc32: number;
  readonly frameId: string;
}

let wholeScanCrcInvocations = 0;
let wholeScanCrcBytesVisited = 0;
let persistenceBytesCopied = 0;
let controllerOwnedCopies = 0;
let controllerOwnedBytesCopied = 0;
let apiBoundaryCopies = 0;
let apiBoundaryBytesCopied = 0;

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

export function recordControllerOwnedScanCopy(byteLength: number): void {
  controllerOwnedCopies += 1;
  controllerOwnedBytesCopied += byteLength;
  persistenceBytesCopied += byteLength;
}

export function recordApiBoundaryCopy(byteLength: number): void {
  apiBoundaryCopies += 1;
  apiBoundaryBytesCopied += byteLength;
  persistenceBytesCopied += byteLength;
}

export function getScanIdentityMetrics(): Readonly<{
  wholeScanCrcInvocations: number;
  wholeScanCrcBytesVisited: number;
  persistenceBytesCopied: number;
  controllerOwnedCopies: number;
  controllerOwnedBytesCopied: number;
  apiBoundaryCopies: number;
  apiBoundaryBytesCopied: number;
}> {
  return {
    wholeScanCrcInvocations,
    wholeScanCrcBytesVisited,
    persistenceBytesCopied,
    controllerOwnedCopies,
    controllerOwnedBytesCopied,
    apiBoundaryCopies,
    apiBoundaryBytesCopied,
  };
}

export function resetScanIdentityMetrics(): void {
  wholeScanCrcInvocations = 0;
  wholeScanCrcBytesVisited = 0;
  persistenceBytesCopied = 0;
  controllerOwnedCopies = 0;
  controllerOwnedBytesCopied = 0;
  apiBoundaryCopies = 0;
  apiBoundaryBytesCopied = 0;
}
