/// <reference lib="webworker" />

import {
  finalizeDirectionalTsdfPlan,
  type DirectionalTsdfPlanSummary,
} from './directionalTsdf';
import type { SparseTsdfMesh } from './sparseTsdf';

interface MergeMessage {
  type: 'merge';
  requestId: number;
  summary: DirectionalTsdfPlanSummary;
  meshes: SparseTsdfMesh[];
  totalComputeMs: number;
}

self.addEventListener('message', (event: MessageEvent<MergeMessage>) => {
  const message = event.data;
  if (message?.type !== 'merge') return;
  const result = finalizeDirectionalTsdfPlan(
    message.summary,
    message.meshes,
    message.totalComputeMs,
  );
  self.postMessage({ type: 'merge-result', requestId: message.requestId, result }, [result.trianglePositions.buffer]);
});

export {};
