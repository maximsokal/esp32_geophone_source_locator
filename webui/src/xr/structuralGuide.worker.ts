/// <reference lib="webworker" />

import { StructuralGuidePipeline, type WorkerMessage } from './structuralGuidePipeline';

const pipeline = new StructuralGuidePipeline();
let currentEpoch = 0;

self.addEventListener('message', (event: MessageEvent<WorkerMessage>) => {
  const message = event.data;
  if (message.type === 'reset') {
    currentEpoch = message.epoch;
    pipeline.reset();
    return;
  }
  if (message.epoch !== currentEpoch) return;
  const { result, diagnostics } = pipeline.analyze(message);
  self.postMessage({
    type: 'result',
    requestId: message.requestId,
    epoch: message.epoch,
    result,
    diagnostics,
  });
});

export {};
