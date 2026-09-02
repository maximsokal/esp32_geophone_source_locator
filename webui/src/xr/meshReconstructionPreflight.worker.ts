/// <reference lib="webworker" />

import {
  assessMeshReconstructionPreflight,
  type MeshReconstructionPreflight,
} from './meshReconstructionPreflight';

interface PreflightMessage {
  type: 'preflight';
  requestId: number;
  positions: Float32Array;
  normals: Float32Array;
  confidence: Float32Array;
  voxelSizeMeters: number;
}

self.addEventListener('message', (event: MessageEvent<PreflightMessage>) => {
  const message = event.data;
  if (message?.type !== 'preflight') return;
  const started = performance.now();
  const result: MeshReconstructionPreflight = assessMeshReconstructionPreflight({
    positions: message.positions,
    normals: message.normals,
    confidence: message.confidence,
    voxelSizeMeters: message.voxelSizeMeters,
  });
  const computeMs = performance.now() - started;
  const transfers: Transferable[] = [
    message.positions.buffer,
    message.normals.buffer,
    message.confidence.buffer,
    result.analysis.eligiblePointMask.buffer,
  ];
  if (result.analysis.componentIdByPoint !== undefined) {
    transfers.push(result.analysis.componentIdByPoint.buffer);
  }
  self.postMessage({
    type: 'preflight-result',
    requestId: message.requestId,
    positions: message.positions,
    normals: message.normals,
    confidence: message.confidence,
    result,
    computeMs,
  }, transfers);
});

export {};
