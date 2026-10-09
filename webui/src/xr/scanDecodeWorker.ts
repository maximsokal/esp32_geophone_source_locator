/// <reference lib="webworker" />

import { decodeScanArtifactWithIdentityAndMetrics, type ScanArtifactDecodingMetrics } from './scanArtifact';
import type { ScanIdentity } from './scanIdentity';
import type { ScanPointCloudSnapshot } from './webxrRuntime';

export interface ScanDecodeWorkerRequest {
  type: 'decode';
  requestId: number;
  generation: number;
  artifact: ArrayBuffer;
  bytesCopied: number;
}

export interface ScanDecodeCounters extends ScanArtifactDecodingMetrics {
  /** Bytes copied at the caller boundary when the source buffer could not be transferred. */
  bytesCopied: number;
}

export type ScanDecodeWorkerResponse =
  | {
    type: 'result';
    requestId: number;
    generation: number;
    snapshot: ScanPointCloudSnapshot;
    identity: ScanIdentity;
    counters: ScanDecodeCounters;
  }
  | { type: 'error'; requestId: number; generation: number; error: string };

interface WorkerMessagePort {
  postMessage(message: ScanDecodeWorkerResponse, transfer?: Transferable[]): void;
}

function snapshotBuffers(snapshot: ScanPointCloudSnapshot): ArrayBuffer[] {
  const buffers = new Set<ArrayBuffer>();
  for (const value of Object.values(snapshot)) {
    if (!(value instanceof Float32Array || value instanceof Uint8Array)) continue;
    if (value.buffer instanceof ArrayBuffer) buffers.add(value.buffer);
  }
  return [...buffers];
}

export function scanDecodeResponseTransferables(response: ScanDecodeWorkerResponse): Transferable[] {
  return response.type === 'result' ? snapshotBuffers(response.snapshot) : [];
}

/** Shared worker entry logic, also used by the in-process protocol harness. */
export function processScanDecodeWorkerMessage(message: ScanDecodeWorkerRequest, port: WorkerMessagePort): void {
  try {
    const decoded = decodeScanArtifactWithIdentityAndMetrics(message.artifact);
    const response: ScanDecodeWorkerResponse = {
      type: 'result',
      requestId: message.requestId,
      generation: message.generation,
      snapshot: decoded.snapshot,
      identity: decoded.identity,
      counters: { ...decoded.metrics, bytesCopied: message.bytesCopied },
    };
    port.postMessage(response, scanDecodeResponseTransferables(response));
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
  workerScope.addEventListener('message', (event: MessageEvent<ScanDecodeWorkerRequest>) => {
    processScanDecodeWorkerMessage(event.data, workerScope);
  });
}
