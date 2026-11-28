import DepthFusionWorker from './depthFusion.worker?worker&inline';
import { createInPageCheckpointKeeper } from './depthFusionKeeper';

/** The checkpoint keeper (crash recovery). Null where there is no Worker: there is then no fusion worker to recover. */
export function createDepthFusionKeeper(): Worker | null {
  return typeof Worker === 'undefined' ? null : createInPageCheckpointKeeper();
}

/** The production worker. Null where there is no Worker (tests, very old WebViews): the client then runs the same core inline. */
export function createDepthFusionWorker(): Worker | null {
  return typeof Worker === 'undefined' ? null : new DepthFusionWorker();
}
