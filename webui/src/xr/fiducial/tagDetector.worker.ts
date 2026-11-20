/// <reference lib="webworker" />
import { chooseDetectionPass, detectAprilTagsPassDetailed } from './tagDetector';

interface ResetMessage { type: 'reset'; epoch: number; }
interface DetectMessage {
  type: 'detect'; requestId: number; epoch: number; width: number; height: number;
  capturedAtMs: number; rgba: Uint8Array;
}
type TagWorkerRequest = ResetMessage | DetectMessage;
let activeEpoch = 0;
let frameIndex = 0;

self.addEventListener('message', (event: MessageEvent<TagWorkerRequest>) => {
  const request = event.data;
  if (request.type === 'reset') { activeEpoch = request.epoch; frameIndex = 0; return; }
  if (request.epoch !== activeEpoch) return;
  const requested = chooseDetectionPass(frameIndex);
  frameIndex += 1;
  const started = performance.now();
  const { detections, used: pass } = detectAprilTagsPassDetailed(requested, request.width, request.height, request.rgba);
  const workerMs = performance.now() - started;
  if (request.epoch === activeEpoch) self.postMessage({
    type: 'result', requestId: request.requestId, epoch: request.epoch,
    capturedAtMs: request.capturedAtMs, detections, pass, workerMs,
  });
});

export {};
