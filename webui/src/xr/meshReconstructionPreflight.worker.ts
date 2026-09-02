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

function errorText(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

self.addEventListener('message', (event: MessageEvent<PreflightMessage>) => {
  const message = event.data;
  if (message?.type !== 'preflight') return;
  const started = performance.now();
  try {
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
  } catch (error) {
    // Ownership must always return to the caller. Otherwise an exception after
    // postMessage transfer would strand detached surfel buffers on the main side.
    self.postMessage({
      type: 'preflight-error',
      requestId: message.requestId,
      positions: message.positions,
      normals: message.normals,
      confidence: message.confidence,
      computeMs: performance.now() - started,
      error: errorText(error),
    }, [message.positions.buffer, message.normals.buffer, message.confidence.buffer]);
  }
});

export {};
