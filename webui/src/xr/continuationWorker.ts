/// <reference lib="webworker" />

import {
  registerAndMergeScanContinuation,
  type ContinuationBudgetEstimate,
  type ScanContinuationPhase,
  type ScanContinuationResult,
} from './scanContinuation';
import type { ScanPointCloudSnapshot } from './webxrRuntime';

export interface ContinuationWorkerRequest {
  type: 'continue';
  requestId: number;
  generation: number;
  base: ScanPointCloudSnapshot;
  extension: ScanPointCloudSnapshot;
  options: {
    budget: ContinuationBudgetEstimate;
  };
}

export type ContinuationWorkerResponse =
  | { type: 'phase'; requestId: number; generation: number; phase: ScanContinuationPhase }
  | { type: 'result'; requestId: number; generation: number; result: ScanContinuationResult }
  | { type: 'error'; requestId: number; generation: number; error: string };

interface WorkerMessagePort {
  postMessage(message: ContinuationWorkerResponse, transfer?: Transferable[]): void;
}

export function continuationResultTransferables(result: ScanContinuationResult): Transferable[] {
  if (!result.ok) return [];
  const transfer: Transferable[] = [];
  const seen = new Set<ArrayBuffer>();
  for (const value of Object.values(result.snapshot)) {
    if (!ArrayBuffer.isView(value)) continue;
    const buffer = value.buffer;
    if (buffer instanceof ArrayBuffer && !seen.has(buffer)) {
      seen.add(buffer);
      transfer.push(buffer);
    }
  }
  return transfer;
}

/** Shared worker entry logic, also used by the in-process protocol harness. */
export function processContinuationWorkerMessage(
  message: ContinuationWorkerRequest,
  port: WorkerMessagePort,
): void {
  try {
    const result = registerAndMergeScanContinuation(message.base, message.extension, {
      onPhase: (phase) => port.postMessage({
        type: 'phase', requestId: message.requestId, generation: message.generation, phase,
      }),
    });
    port.postMessage({ type: 'result', requestId: message.requestId, generation: message.generation, result },
      continuationResultTransferables(result));
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
  workerScope.addEventListener('message', (event: MessageEvent<ContinuationWorkerRequest>) => {
    processContinuationWorkerMessage(event.data, workerScope);
  });
}
