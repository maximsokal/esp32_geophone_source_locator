/**
 * One-tap scan start (issue #21).
 *
 * The first "3D scan" tap navigates to the scanner screen, whose runtime owner (XrRuntimePanel) mounts only after that. The tap's browser
 * user activation, however, is still live for a few seconds, and navigator.userActivation.isActive is exactly what the browser checks in
 * requestSession('immersive-ar'). So the tap leaves a single-use INTENT here, and the scanner starts the Fresh scan itself only while:
 *   - the activation is still active at the moment it consumes the intent (no fake activation: with it gone the normal Start button stays),
 *   - the intent is young enough, belongs to this object/session, and was not used before (a remount or a second render cannot repeat it),
 *   - the saved-scan restore finished and found nothing (a saved scan is never started over, restored scans open as viewer).
 */

/** Chrome's transient activation lasts about 5 s; stay clearly inside it. */
export const SCAN_LAUNCH_INTENT_MAX_AGE_MS = 4000;

/** The workflow scans before a recording exists: its scanner has no session id and stores the scan in staging. */
export const SCAN_LAUNCH_STAGING_SESSION = 'staging';

interface ActivationNavigator { userActivation?: { isActive?: boolean } }

interface ScanLaunchIntent { objectId: string; sessionId: string; requestedAtMs: number }

let pending: ScanLaunchIntent | null = null;

function activationIsActive(): boolean {
  if (typeof navigator === 'undefined') return false;
  return (navigator as ActivationNavigator).userActivation?.isActive === true;
}

/** Called from the tap handler. Without a live activation nothing is remembered and the scanner shows its Start button as before. */
export function requestScanLaunch(objectId: string, sessionId: string, nowMs: number = Date.now()): boolean {
  if (!activationIsActive()) {
    pending = null;
    return false;
  }
  pending = { objectId, sessionId, requestedAtMs: nowMs };
  return true;
}

/** Single use: the intent is gone after this call whatever it answers. True only when the launch may start right now. */
export function consumeScanLaunchIntent(objectId: string, sessionId: string, nowMs: number = Date.now()): boolean {
  const intent = pending;
  pending = null;
  if (intent === null) return false;
  if (intent.objectId !== objectId || intent.sessionId !== sessionId) return false;
  if (nowMs - intent.requestedAtMs > SCAN_LAUNCH_INTENT_MAX_AGE_MS) return false;
  return activationIsActive();
}

export function clearScanLaunchIntent(): void {
  pending = null;
}

export function hasScanLaunchIntent(): boolean {
  return pending !== null;
}
