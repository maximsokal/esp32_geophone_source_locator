/// <reference lib="webworker" />

import { localizationKernelBase64 } from './localizationKernelBinary.generated';
import { LocalizationKernel, type KernelRecording, type KernelResult } from './localizationKernel';

/** The calculation takes seconds on a long recording: it runs here so the page stays responsive. */
export interface KernelWorkerRequest {
  type: 'run';
  requestId: number;
  objectId: string;
  sessionId: string;
  recording: KernelRecording;
  geometryJson: string;
  configJson: string;
  computedAtMs: number;
}

export type KernelWorkerResponse =
  | { type: 'result'; requestId: number; result: KernelResult }
  | { type: 'error'; requestId: number; error: string };

function decodeBase64(text: string): Uint8Array<ArrayBuffer> {
  const binary = atob(text);
  const bytes = new Uint8Array(new ArrayBuffer(binary.length));
  for (let i = 0; i < binary.length; i += 1) bytes[i] = binary.charCodeAt(i);
  return bytes;
}

let kernel: Promise<LocalizationKernel> | null = null;

self.onmessage = (event: MessageEvent<KernelWorkerRequest>) => {
  const request = event.data;
  if (request.type !== 'run') return;
  kernel ??= LocalizationKernel.load(decodeBase64(localizationKernelBase64));
  kernel
    .then((loaded) => {
      const result = loaded.run(request);
      (self as unknown as Worker).postMessage({ type: 'result', requestId: request.requestId, result } satisfies KernelWorkerResponse);
    })
    .catch((reason: unknown) => {
      (self as unknown as Worker).postMessage({
        type: 'error', requestId: request.requestId, error: reason instanceof Error ? reason.message : String(reason),
      } satisfies KernelWorkerResponse);
    });
};
