import { fetchSavedSeismicSession } from '../api/deviceApi';
import type {
  LocalizationPickChannel,
  LocalizationRunPosition,
  LocalizationSessionPodGeometry,
} from '../api/deviceApi';
import type { SavedSeismicSnapshot } from './device';

// Tune these defaults with real recordings; C mirrors the same values.
export const RESULT_VIEW_MINIMUM_WINDOW_S = 2.0;
export const RESULT_VIEW_DEFAULT_PRE_S = 0.5;
export const RESULT_VIEW_DEFAULT_POST_S = 1.0;

export interface ResultSegmentWindow {
  available: boolean;
  start_frame: number;
  frame_count: number;
}

export type ResultSegmentPick = Pick<LocalizationPickChannel, 'arrival_sample_index' | 'status' | 'snr'>;

function requireWindowInputs(
  picks: readonly ResultSegmentPick[],
  sampleRateHz: number,
  sessionFrameCount: number,
  preSeconds: number,
  postSeconds: number,
): void {
  if (
    !Number.isSafeInteger(sampleRateHz) || sampleRateHz <= 0 ||
    !Number.isSafeInteger(sessionFrameCount) || sessionFrameCount <= 0 ||
    !Number.isFinite(preSeconds) || preSeconds < 0 ||
    !Number.isFinite(postSeconds) || postSeconds < 0
  ) {
    throw new RangeError('Invalid result segment window inputs.');
  }
  if (
    !Number.isFinite(preSeconds * sampleRateHz) || preSeconds * sampleRateHz > Number.MAX_SAFE_INTEGER ||
    !Number.isFinite(postSeconds * sampleRateHz) || postSeconds * sampleRateHz > Number.MAX_SAFE_INTEGER ||
    !Number.isSafeInteger(Math.ceil(RESULT_VIEW_MINIMUM_WINDOW_S * sampleRateHz))
  ) {
    throw new RangeError('Result segment window exceeds safe frame precision.');
  }
  for (const pick of picks) {
    if (pick.status === 'accepted' && (
      !Number.isSafeInteger(pick.arrival_sample_index) ||
      pick.arrival_sample_index < 0 || pick.arrival_sample_index >= sessionFrameCount
    )) {
      throw new RangeError('Accepted pick lies outside the saved session.');
    }
  }
}

export function resultViewSegmentWindow(
  picks: readonly ResultSegmentPick[],
  sampleRateHz: number,
  sessionFrameCount: number,
  preSeconds = RESULT_VIEW_DEFAULT_PRE_S,
  postSeconds = RESULT_VIEW_DEFAULT_POST_S,
): ResultSegmentWindow {
  requireWindowInputs(picks, sampleRateHz, sessionFrameCount, preSeconds, postSeconds);

  const accepted = picks
    .filter((pick) => pick.status === 'accepted')
    .map((pick) => pick.arrival_sample_index);
  if (accepted.length === 0) {
    return { available: false, start_frame: 0, frame_count: 0 };
  }

  const firstArrival = Math.min(...accepted);
  const lastArrival = Math.max(...accepted);
  const requestedStart = firstArrival - preSeconds * sampleRateHz;
  const requestedEnd = lastArrival + postSeconds * sampleRateHz;
  const start = requestedStart <= 0 ? 0 : Math.floor(requestedStart);
  let end = requestedEnd >= sessionFrameCount ? sessionFrameCount : Math.ceil(requestedEnd);
  if (end <= lastArrival) end = lastArrival + 1;

  const minimumFrameCount = Math.ceil(RESULT_VIEW_MINIMUM_WINDOW_S * sampleRateHz);
  if (sessionFrameCount <= minimumFrameCount) {
    return { available: true, start_frame: 0, frame_count: sessionFrameCount };
  }

  let startFrame = start;
  let frameCount = end - start;
  if (frameCount < minimumFrameCount) {
    const leftExpansion = Math.floor((minimumFrameCount - frameCount) / 2);
    startFrame = start > leftExpansion ? start - leftExpansion : 0;
    const latestStart = sessionFrameCount - minimumFrameCount;
    if (startFrame > latestStart) startFrame = latestStart;
    frameCount = minimumFrameCount;
  }

  return { available: true, start_frame: startFrame, frame_count: frameCount };
}

export function resultViewArrivalMarkerOffset(
  pick: ResultSegmentPick,
  window: ResultSegmentWindow,
): number | null {
  if (
    !window.available || window.frame_count <= 0 || pick.status !== 'accepted' ||
    !Number.isSafeInteger(pick.arrival_sample_index) ||
    pick.arrival_sample_index < window.start_frame
  ) {
    return null;
  }
  const offset = pick.arrival_sample_index - window.start_frame;
  return offset < window.frame_count ? offset : null;
}

export function resultViewSourceDistances(
  source: LocalizationRunPosition,
  pods: readonly LocalizationSessionPodGeometry[],
): (number | null)[] {
  if (pods.length > 4 || ![source.x_m, source.y_m, source.z_m].every(Number.isFinite)) {
    throw new RangeError('Invalid source geometry.');
  }
  return pods.map((pod) => {
    if (!pod.computed) return null;
    const xMm = pod.x_mm;
    const yMm = pod.y_mm;
    const zMm = pod.z_mm;
    if (
      typeof xMm !== 'number' || !Number.isFinite(xMm) ||
      typeof yMm !== 'number' || !Number.isFinite(yMm) ||
      typeof zMm !== 'number' || !Number.isFinite(zMm)
    ) {
      throw new RangeError('Computed pod geometry is incomplete.');
    }
    return Math.hypot(
      source.x_m - xMm / 1000,
      source.y_m - yMm / 1000,
      source.z_m - zMm / 1000,
    );
  });
}

export async function fetchResultSegment(
  objectId: string,
  sessionId: string,
  picks: readonly ResultSegmentPick[],
  sampleRateHz: number,
  sessionFrameCount: number,
  preSeconds = RESULT_VIEW_DEFAULT_PRE_S,
  postSeconds = RESULT_VIEW_DEFAULT_POST_S,
): Promise<{ window: ResultSegmentWindow; session: SavedSeismicSnapshot } | null> {
  const window = resultViewSegmentWindow(picks, sampleRateHz, sessionFrameCount, preSeconds, postSeconds);
  if (!window.available) return null;
  const session = await fetchSavedSeismicSession(objectId, sessionId, {
    startFrame: window.start_frame,
    frameCount: window.frame_count,
  });
  return { window, session };
}
