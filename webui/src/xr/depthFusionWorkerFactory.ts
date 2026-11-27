import DepthFusionWorker from './depthFusion.worker?worker&inline';

/** The production worker. Null where there is no Worker (tests, very old WebViews): the client then runs the same core inline. */
export function createDepthFusionWorker(): Worker | null {
  return typeof Worker === 'undefined' ? null : new DepthFusionWorker();
}
