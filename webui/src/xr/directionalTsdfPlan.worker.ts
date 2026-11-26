/// <reference lib="webworker" />

import {
  finalizeDirectionalTsdfPlan,
  prepareHybridTsdfPlan,
  type DirectionalTsdfPlanSummary,
} from './directionalTsdf';
import { buildSparseTsdfMesh, type SparseTsdfInput } from './sparseTsdf';

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

  const plan = prepareHybridTsdfPlan(message.input, true);
  const { channelInputs, ...summary } = plan;

  // Flat/single-family scans are common. Finish them in the planner Worker so
  // they pay for only one Worker startup instead of planner -> channel -> merge.
  if (summary.mode !== 'multi') {
    const meshes = channelInputs.map((input) => buildSparseTsdfMesh(input));
    const result = finalizeDirectionalTsdfPlan(summary, meshes);
    self.postMessage({
      type: 'plan-direct-result',
      requestId: message.requestId,
      summary: summary satisfies DirectionalTsdfPlanSummary,
      result,
    }, [result.trianglePositions.buffer]);
    return;
  }

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
