/// <reference lib="webworker" />

import { detectVisualLines } from './visualLineEvidence';
import type { RgbDepthGuidanceFrame } from './rgbDepthGuidance';

interface AnalyzeMessage {
  type: 'analyze';
  requestId: number;
  epoch: number;
  width: number;
  height: number;
  capturedAtMs: number;
  edgeStrength: Uint8Array;
  gradientX: Int8Array;
  gradientY: Int8Array;
}

interface ResetMessage {
  type: 'reset';
  epoch: number;
}

type WorkerMessage = AnalyzeMessage | ResetMessage;

let activeEpoch = 0;

self.addEventListener('message', (event: MessageEvent<WorkerMessage>) => {
  const message = event.data;
  if (message.type === 'reset') {
    activeEpoch = message.epoch;
    return;
  }
  if (message.epoch !== activeEpoch) return;

  // The detector needs only Sobel evidence. Luma is deliberately not copied to
  // the worker, reducing transfer bandwidth while keeping RGB metric-free.
  const frame: RgbDepthGuidanceFrame = {
    width: message.width,
    height: message.height,
    luma: new Uint8Array(message.width * message.height),
    edgeStrength: message.edgeStrength,
    gradientX: message.gradientX,
    gradientY: message.gradientY,
    capturedAtMs: message.capturedAtMs,
  };
  const detected = detectVisualLines(frame);
  if (message.epoch !== activeEpoch) return;
  self.postMessage({
    type: 'result',
    requestId: message.requestId,
    epoch: message.epoch,
    lines: detected.lines,
    junctions: detected.junctions,
  });
});

export {};
