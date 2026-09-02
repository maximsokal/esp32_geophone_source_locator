/// <reference lib="webworker" />

import { buildSparseTsdfMesh, type SparseTsdfInput } from './sparseTsdf';

interface ReconstructChannelMessage {
  type: 'reconstruct-channel';
  requestId: number;
  channelIndex: number;
  input: SparseTsdfInput;
}

self.addEventListener('message', (event: MessageEvent<ReconstructChannelMessage>) => {
  const message = event.data;
  if (message?.type !== 'reconstruct-channel') return;
  const result = buildSparseTsdfMesh(message.input);
  self.postMessage({
    type: 'channel-result',
    requestId: message.requestId,
    channelIndex: message.channelIndex,
    result,
  }, [result.trianglePositions.buffer]);
});

export {};
