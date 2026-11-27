/// <reference lib="webworker" />

import { createCheckpointKeeper, type KeeperRequest, type PortLike } from './depthFusionKeeper';

if (typeof self !== 'undefined') {
  const scope = self as DedicatedWorkerGlobalScope;
  const { handle } = createCheckpointKeeper((message) => scope.postMessage(message));
  scope.addEventListener('message', (event: MessageEvent<KeeperRequest>) => {
    const message = event.data;
    handle(message.type === 'drop' ? message : { ...message, port: (event.ports[0] ?? message.port) as unknown as PortLike });
  });
}
