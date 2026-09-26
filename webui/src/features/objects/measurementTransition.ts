import type { DeviceStatus } from '../../domain/device';

export type MeasurementTransitionAction = 'measurement_start' | 'measurement_stop';

export const MEASUREMENT_TRANSITION_POLL_MS = 250;
export const MEASUREMENT_TRANSITION_TIMEOUT_MS = 30_000;
export const STORAGE_CACHE_IDLE_TIMEOUT_MS = 30_000;

type ReadStatus = () => Promise<DeviceStatus>;

export interface MeasurementTransitionWaitOptions {
  intervalMs?: number;
  timeoutMs?: number;
  now?: () => number;
  sleep?: (milliseconds: number) => Promise<void>;
}

function defaultSleep(milliseconds: number): Promise<void> {
  return new Promise((resolve) => {
    globalThis.setTimeout(resolve, milliseconds);
  });
}

function measurementState(status: DeviceStatus): string | undefined {
  /* New firmware exposes the service state explicitly. */
  if (status.measurement !== undefined) return status.measurement.state;
  /* Keep older firmware usable while it is being updated. */
  return status.recording?.state;
}

export function measurementTransitionTarget(
  action: MeasurementTransitionAction,
): 'RECORDING' | 'REVIEW' {
  return action === 'measurement_start' ? 'RECORDING' : 'REVIEW';
}

export function measurementTransitionNeedsSessionRefresh(
  action: MeasurementTransitionAction,
): boolean {
  /* A session is durable only after STOP has completed metadata finalization. */
  return action === 'measurement_stop';
}

export function measurementOwnsRecorder(status: DeviceStatus): boolean {
  const state = status.measurement?.state;
  if (
    state === 'STARTING' ||
    state === 'RECORDING' ||
    state === 'STOPPING'
  ) {
    return true;
  }

  /* Keep the UI usable with firmware that predates measurement.state. */
  return status.measurement === undefined && status.recording?.running === true;
}

export function scannerStorageActionsDisabled(status: DeviceStatus | null): boolean {
  return status !== null && measurementOwnsRecorder(status);
}

export function measurementControlLocked(status: DeviceStatus): boolean {
  return measurementOwnsRecorder(status) || status.measurement?.busy === true;
}

export function isStorageCacheIdle(status: DeviceStatus): boolean {
  /* Older firmware has no cache field; keep it usable while upgrading. */
  return status.storage_busy !== true;
}

export async function waitForStorageCacheIdle(
  readStatus: ReadStatus,
  options: MeasurementTransitionWaitOptions = {},
): Promise<DeviceStatus> {
  const intervalMs = Math.max(
    1,
    Math.round(options.intervalMs ?? MEASUREMENT_TRANSITION_POLL_MS),
  );
  const timeoutMs = Math.max(
    intervalMs,
    Math.round(options.timeoutMs ?? STORAGE_CACHE_IDLE_TIMEOUT_MS),
  );
  const now = options.now ?? (() => Date.now());
  const sleep = options.sleep ?? defaultSleep;
  const deadline = now() + timeoutMs;

  while (true) {
    const status = await readStatus();
    if (isStorageCacheIdle(status)) return status;

    const remaining = deadline - now();
    if (remaining <= 0) {
      throw new Error(
        `ESP storage cache did not become idle within ${timeoutMs} ms.`,
      );
    }
    await sleep(Math.min(intervalMs, remaining));
  }
}

export function isMeasurementTransitionComplete(
  action: MeasurementTransitionAction,
  status: DeviceStatus,
): boolean {
  const state = measurementState(status);
  if (state === measurementTransitionTarget(action)) return true;

  /* Fallback for a pre-transition firmware status without measurement. */
  return action === 'measurement_stop' &&
    status.measurement === undefined &&
    status.recording?.state === 'IDLE' &&
    status.recording.stop_stage === 'complete';
}

export function measurementTransitionErrorMessage(status: DeviceStatus): string | null {
  const measurement = status.measurement;
  if (measurement?.state === 'ERROR') {
    return measurement.last_error_name ?? measurement.error ?? 'measurement service error';
  }

  const recording = status.recording;
  if (measurement === undefined && recording?.state === 'ERROR') {
    return recording.last_error_name ?? recording.error ?? 'recorder error';
  }

  return null;
}

export async function waitForMeasurementTransition(
  action: MeasurementTransitionAction,
  readStatus: ReadStatus,
  options: MeasurementTransitionWaitOptions = {},
): Promise<DeviceStatus> {
  const intervalMs = Math.max(
    1,
    Math.round(options.intervalMs ?? MEASUREMENT_TRANSITION_POLL_MS),
  );
  const timeoutMs = Math.max(
    intervalMs,
    Math.round(options.timeoutMs ?? MEASUREMENT_TRANSITION_TIMEOUT_MS),
  );
  const now = options.now ?? (() => Date.now());
  const sleep = options.sleep ?? defaultSleep;
  const deadline = now() + timeoutMs;
  const target = measurementTransitionTarget(action);

  while (true) {
    const status = await readStatus();
    if (isMeasurementTransitionComplete(action, status)) return status;

    /* A completed ERROR is terminal; STOP callers refresh the session model
     * before surfacing the asynchronous failure. */
    const error = measurementTransitionErrorMessage(status);
    /*
     * START is also accepted from the service's ERROR state. The submit gate
     * is taken before the worker changes ERROR to STARTING, so an immediate
     * status response may still contain the previous error. While busy is
     * true the command is still in flight; only ERROR with the gate released
     * belongs to this transition.
     */
    if (error !== null && status.measurement?.busy !== true) {
      return status;
    }

    if (status.measurement?.available === false) {
      throw new Error(
        `ESP ${action} status became unavailable before ${target}: ` +
        (status.measurement.error ?? 'measurement service status unavailable'),
      );
    }

    const remaining = deadline - now();
    if (remaining <= 0) {
      throw new Error(`ESP ${action} did not reach ${target} within ${timeoutMs} ms.`);
    }
    await sleep(Math.min(intervalMs, remaining));
  }
}
