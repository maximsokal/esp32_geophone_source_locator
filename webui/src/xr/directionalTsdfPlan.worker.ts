/// <reference lib="webworker" />

import {
  prepareDirectionalTsdfPlan,
  type DirectionalTsdfPlanSummary,
} from './directionalTsdf';
import type { SparseTsdfInput } from './sparseTsdf';

interface PlanMessage {
  type: 'plan';
  requestId: number;
  input: SparseTsdfInput;
}

function transferBuffer(
  transfers: Transferable[],
  seen: Set<ArrayBuffer>,
  buffer: ArrayBufferLike,
): void {
  if (!(buffer instanceof ArrayBuffer) || seen.has(buffer)) return;
  seen.add(buffer);
  transfers.push(buffer);
}

self.addEventListener('message', (event: MessageEvent<PlanMessage>) => {
  const message = event.data;
  if (message?.type !== 'plan') return;

  const plan = prepareDirectionalTsdfPlan(message.input, true);
  const { channelInputs, ...summary } = plan;
  const transfers: Transferable[] = [];
  const seen = new Set<ArrayBuffer>();
  for (const input of channelInputs) {
    transferBuffer(transfers, seen, input.positions.buffer);
    transferBuffer(transfers, seen, input.normals.buffer);
    transferBuffer(transfers, seen, input.confidence.buffer);
    if (input.freeSpacePositions !== undefined) transferBuffer(transfers, seen, input.freeSpacePositions.buffer);
  }

  self.postMessage({
    type: 'plan-result',
    requestId: message.requestId,
    summary: summary satisfies DirectionalTsdfPlanSummary,
    channelInputs,
  }, transfers);
});

export {};
