export type DepthSensingPauseAction = 'pause' | 'resume' | null;

/**
 * Pure policy for the WebXR depth pause controller.
 *
 * The WebXR Depth Sensing specification requires pauseDepthSensing() and
 * resumeDepthSensing() to run while the XRSession animation frame is active.
 * Visibility handlers therefore only update the desired scanPaused state;
 * WebXrRuntime evaluates this policy from inside its XR RAF callback.
 */
export function depthSensingPauseAction(
  depthEnabled: boolean,
  desiredPaused: boolean,
  appliedPaused: boolean,
): DepthSensingPauseAction {
  if (!depthEnabled || desiredPaused === appliedPaused) return null;
  return desiredPaused ? 'pause' : 'resume';
}

/** Keep document and XR-session visibility semantics in one testable place. */
export function shouldPauseXrScan(
  documentVisibility: DocumentVisibilityState,
  sessionVisibility: 'visible' | 'visible-blurred' | 'hidden' | undefined,
): boolean {
  return documentVisibility !== 'visible' || sessionVisibility === 'hidden';
}
