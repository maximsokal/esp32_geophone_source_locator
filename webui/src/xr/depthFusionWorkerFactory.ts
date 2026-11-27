import DepthFusionWorker from './depthFusion.worker?worker&inline';
import DepthFusionKeeperWorker from './depthFusionKeeper.worker?worker&inline';

/** The checkpoint keeper (crash recovery). Null where there is no Worker. */
export function createDepthFusionKeeper(): Worker | null {
  return typeof Worker === 'undefined' ? null : new DepthFusionKeeperWorker();
}

/** The production worker. Null where there is no Worker (tests, very old WebViews): the client then runs the same core inline. */
export function createDepthFusionWorker(): Worker | null {
  return typeof Worker === 'undefined' ? null : new DepthFusionWorker();
}
