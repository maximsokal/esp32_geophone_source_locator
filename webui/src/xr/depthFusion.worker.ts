/// <reference lib="webworker" />

import { createDepthFusionHandler } from './depthFusionCore';
import type { FusionWorkerRequest } from './depthFusionProtocol';

/**
 * Persistent depth-fusion worker (Phase 2): owns the accumulated surface and occupancy state of the running scan. One instance per XR session; it lives
 * until the session's scan is finished or abandoned. All logic is in depthFusionCore so that it is testable without a Worker.
 */
if (typeof self !== 'undefined') {
  const scope = self as DedicatedWorkerGlobalScope;
  const { handle } = createDepthFusionHandler((message, transfer) => scope.postMessage(message, transfer ?? []));
  scope.addEventListener('message', (event: MessageEvent<FusionWorkerRequest>) => handle(event.data));
}
