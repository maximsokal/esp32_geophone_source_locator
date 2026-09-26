/// <reference lib="webworker" />
import { detectAprilTags } from './tagDetector';

interface ResetMessage { type: 'reset'; epoch: number; }
interface DetectMessage {
  type: 'detect'; requestId: number; epoch: number; width: number; height: number;
  capturedAtMs: number; rgba: Uint8Array;
}
type TagWorkerRequest = ResetMessage | DetectMessage;
let activeEpoch = 0;

self.addEventListener('message', (event: MessageEvent<TagWorkerRequest>) => {
  const request = event.data;
  if (request.type === 'reset') { activeEpoch = request.epoch; return; }
  if (request.epoch !== activeEpoch) return;
  const detections = detectAprilTags(request.width, request.height, request.rgba);
  if (request.epoch === activeEpoch) self.postMessage({
    type: 'result', requestId: request.requestId, epoch: request.epoch,
    capturedAtMs: request.capturedAtMs, detections,
  });
});

export {};
