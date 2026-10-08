/// <reference lib="webworker" />

import {
  encodeScanArtifactWithIdentityAndMetrics,
  scanArtifactSnapshotArrayBytes,
  type ScanArtifactEncodingMetrics,
} from './scanArtifact';
import type { ScanIdentity } from './scanIdentity';
import type { ScanPointCloudSnapshot } from './webxrRuntime';

export interface ScanEncodeWorkerRequest {
  type: 'encode';
  requestId: number;
  generation: number;
  snapshot: ScanPointCloudSnapshot;
  copyBytes: number;
}

export interface ScanEncodeCounters extends ScanArtifactEncodingMetrics {
  /** Bytes copied from the frozen snapshot into transferable worker-owned arrays. */
  bytesCopied: number;
}

export type ScanEncodeWorkerResponse =
  | {
    type: 'result';
    requestId: number;
    generation: number;
    artifact: ArrayBuffer;
    identity: ScanIdentity;
    counters: ScanEncodeCounters;
  }
  | { type: 'error'; requestId: number; generation: number; error: string };

interface WorkerMessagePort {
  postMessage(message: ScanEncodeWorkerResponse, transfer?: Transferable[]): void;
}

export function scanEncodeResponseTransferables(response: ScanEncodeWorkerResponse): Transferable[] {
  return response.type === 'result' ? [response.artifact] : [];
}

/** Shared worker entry logic, also used by the in-process protocol harness. */
export function processScanEncodeWorkerMessage(message: ScanEncodeWorkerRequest, port: WorkerMessagePort): void {
  try {
    const bytesCopied = scanArtifactSnapshotArrayBytes(message.snapshot);
    if (bytesCopied !== message.copyBytes) throw new Error('Scan encode copy byte count changed before worker processing.');
    const encoded = encodeScanArtifactWithIdentityAndMetrics(message.snapshot);
    const response: ScanEncodeWorkerResponse = {
      type: 'result',
      requestId: message.requestId,
      generation: message.generation,
      artifact: encoded.bytes.buffer as ArrayBuffer,
      identity: encoded.identity,
      counters: { ...encoded.metrics, bytesCopied },
    };
    port.postMessage(response, scanEncodeResponseTransferables(response));
  } catch (reason) {
    port.postMessage({
      type: 'error',
      requestId: message.requestId,
      generation: message.generation,
      error: reason instanceof Error ? reason.message : String(reason),
    });
  }
}

if (typeof self !== 'undefined') {
  const workerScope = self as DedicatedWorkerGlobalScope;
  workerScope.addEventListener('message', (event: MessageEvent<ScanEncodeWorkerRequest>) => {
    processScanEncodeWorkerMessage(event.data, workerScope);
  });
}
