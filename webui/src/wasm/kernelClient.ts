import KernelWorker from './localizationKernel.worker?worker&inline';
import type { KernelWorkerRequest, KernelWorkerResponse } from './localizationKernel.worker';
import type { KernelRunInput, KernelRunner } from './phoneCalculation';

let nextRequestId = 1;

/** One worker per calculation: the module is loaded fresh, the recording is moved (not copied) into it. */
export const runKernelInWorker: KernelRunner = (input: KernelRunInput) => new Promise((resolve, reject) => {
  const worker = new KernelWorker();
  const requestId = nextRequestId++;
  const finish = () => worker.terminate();
  worker.onmessage = (event: MessageEvent<KernelWorkerResponse>) => {
    if (event.data.requestId !== requestId) return;
    finish();
    if (event.data.type === 'result') resolve(event.data.result);
    else reject(new Error(event.data.error));
  };
  worker.onerror = (event) => {
    finish();
    reject(new Error(event.message || 'the calculation worker failed'));
  };
  const request: KernelWorkerRequest = { type: 'run', requestId, ...input };
  worker.postMessage(request, [input.recording.channelUv.buffer, input.recording.gapBefore.buffer]);
});
