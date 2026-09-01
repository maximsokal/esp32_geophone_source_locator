/// <reference lib="webworker" />

import { StructuralGuideTracker, type StructuralGuideInput } from './structuralGuide';

interface AnalyzeMessage {
  type: 'analyze';
  requestId: number;
  positions: Float32Array;
  normals: Float32Array;
  confidence: Float32Array;
  keyframeCount: number;
  voxelSizeMeters: number;
}

interface ResetMessage { type: 'reset'; }
type WorkerMessage = AnalyzeMessage | ResetMessage;

const tracker = new StructuralGuideTracker();

self.addEventListener('message', (event: MessageEvent<WorkerMessage>) => {
  const message = event.data;
  if (message.type === 'reset') {
    tracker.reset();
    return;
  }

  const started = performance.now();
  const input: StructuralGuideInput = {
    positions: message.positions,
    normals: message.normals,
    confidence: message.confidence,
    keyframeCount: message.keyframeCount,
    voxelSizeMeters: message.voxelSizeMeters,
  };
  const result = tracker.update(input, performance.now() - started);
  self.postMessage({ type: 'result', requestId: message.requestId, result });
});

export {};
