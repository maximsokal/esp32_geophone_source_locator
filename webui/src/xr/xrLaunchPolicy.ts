export interface ImmersiveArLaunchCapabilities {
  secureContext: boolean;
  webGl: boolean;
  xrApi: boolean;
  immersiveAr: boolean;
}

export function canAttemptImmersiveAr(
  capabilities: ImmersiveArLaunchCapabilities | null,
): boolean {
  /*
   * Capability probing is advisory and deliberately runs in the background.
   * Do not disable the launch control while that asynchronous probe is still
   * pending: WebXrRuntime.start() performs the authoritative runtime checks.
   */
  if (capabilities === null) return true;

  return capabilities.secureContext
    && capabilities.webGl
    && capabilities.xrApi
    && capabilities.immersiveAr;
}

interface ImmersiveArLaunchOptions<TPrepared> {
  beginScan: () => void;
  startRuntime: () => Promise<void>;
  prepareCompute: () => Promise<TPrepared>;
  onPrepared?: (prepared: TPrepared) => void;
}

/**
 * Start the immersive session synchronously from the user gesture before the
 * first await. Browsers gate immersive-ar requestSession() behind transient
 * user activation, so unrelated async preparation must never run in front of
 * WebXrRuntime.start().
 */
export async function launchImmersiveArFromUserGesture<TPrepared>(
  options: ImmersiveArLaunchOptions<TPrepared>,
): Promise<TPrepared> {
  options.beginScan();

  /*
   * Calling an async function executes its synchronous prefix immediately.
   * WebXrRuntime.start() reaches navigator.xr.requestSession() before its first
   * await, preserving the click/tap activation token.
   */
  const startPromise = options.startRuntime();

  const preparedPromise = options.prepareCompute().then((prepared) => {
    options.onPrepared?.(prepared);
    return prepared;
  });

  const [, prepared] = await Promise.all([startPromise, preparedPromise]);
  return prepared;
}
