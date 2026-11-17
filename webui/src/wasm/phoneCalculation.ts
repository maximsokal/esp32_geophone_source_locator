import {
  downloadSessionSamples,
  fetchSessionExportMeta,
  getCalcSettings,
  uploadPhoneCalculation,
} from '../api/deviceApi';
import type { KernelRecording, KernelResult } from './localizationKernel';
import { parseSamplesFile } from './samplesFile';

/**
 * Calculate a finished session on the phone instead of on the ESP32: download the recording, run the firmware's own
 * calculation (WebAssembly) and send only the result back, to be stored exactly like a result the device made itself.
 */
export type PhoneCalculationStage = 'meta' | 'settings' | 'download' | 'calculate' | 'upload';

export interface KernelRunInput {
  objectId: string;
  sessionId: string;
  recording: KernelRecording;
  geometryJson: string;
  configJson: string;
  computedAtMs: number;
}

/** Runs the calculation somewhere; the page uses a worker, tests run it in place. */
export type KernelRunner = (input: KernelRunInput) => Promise<KernelResult>;

export interface PhoneCalculationDeps {
  fetchMeta: typeof fetchSessionExportMeta;
  fetchSettings: typeof getCalcSettings;
  download: typeof downloadSessionSamples;
  upload: typeof uploadPhoneCalculation;
  now: () => number;
}

const realDeps: PhoneCalculationDeps = {
  fetchMeta: fetchSessionExportMeta,
  fetchSettings: getCalcSettings,
  download: downloadSessionSamples,
  upload: uploadPhoneCalculation,
  now: () => Date.now(),
};

export interface PhoneCalculationOutcome {
  result: KernelResult;
  uploaded: { runs: number; picks: boolean };
  frames: number;
  sampleRateHz: number;
}

/** The ESP-IDF names of the codes the calculation can end with, so the page says more than a number. */
const ESP_ERROR_NAMES: Record<number, string> = {
  257: 'ESP_ERR_NO_MEM', 258: 'ESP_ERR_INVALID_ARG', 259: 'ESP_ERR_INVALID_STATE', 260: 'ESP_ERR_INVALID_SIZE',
  261: 'ESP_ERR_NOT_FOUND', 262: 'ESP_ERR_NOT_SUPPORTED', 263: 'ESP_ERR_TIMEOUT', 264: 'ESP_ERR_INVALID_RESPONSE',
};

export function describeCalculationFailure(phase: 'settings' | 'run', status: number): string {
  const name = ESP_ERROR_NAMES[status];
  const where = phase === 'settings' ? 'the calculation settings were refused' : 'the calculation stopped';
  return `${where} with error ${status}${name ? ` (${name})` : ''}`;
}

export class PhoneCalculationError extends Error {
  constructor(readonly stage: PhoneCalculationStage, message: string) {
    super(message);
  }
}

async function inStage<T>(stage: PhoneCalculationStage, work: () => Promise<T>): Promise<T> {
  try {
    return await work();
  } catch (reason) {
    if (reason instanceof PhoneCalculationError) throw reason;
    throw new PhoneCalculationError(stage, reason instanceof Error ? reason.message : String(reason));
  }
}

export async function calculateOnPhone(
  objectId: string,
  sessionId: string,
  runKernel: KernelRunner,
  options: {
    onStage?: (stage: PhoneCalculationStage) => void;
    onDownloadProgress?: (receivedBytes: number, totalBytes: number) => void;
    deps?: Partial<PhoneCalculationDeps>;
  } = {},
): Promise<PhoneCalculationOutcome> {
  const deps = { ...realDeps, ...options.deps };
  const stage = options.onStage ?? (() => undefined);

  stage('meta');
  const meta = await inStage('meta', () => deps.fetchMeta(objectId, sessionId));

  stage('settings');
  const configJson = await inStage('settings', async () => {
    const response = await deps.fetchSettings(objectId);
    if (response.settings === null) throw new Error('the object has no valid calculation settings');
    return JSON.stringify(response.settings);
  });

  stage('download');
  const recording = await inStage('download', async () => {
    const parsed = parseSamplesFile(await deps.download(objectId, sessionId, options.onDownloadProgress));
    if (parsed.sampleRateHz !== meta.sampleRateHz) throw new Error('the file and the session disagree on the sample rate');
    if (parsed.gapBefore.length !== meta.frames) {
      throw new Error(`the file holds ${parsed.gapBefore.length} frames, the session ${meta.frames}`);
    }
    return parsed;
  });

  stage('calculate');
  const result = await inStage('calculate', async () => {
    const outcome = await runKernel({
      objectId, sessionId, recording, geometryJson: JSON.stringify(meta.geometry), configJson, computedAtMs: deps.now(),
    });
    if (outcome.status !== 0) throw new Error(describeCalculationFailure(outcome.phase, outcome.status));
    if (outcome.runs.length === 0) throw new Error('the calculation produced no records');
    return outcome;
  });

  stage('upload');
  const uploaded = await inStage('upload', async () => {
    const reply = await deps.upload({ objectId, sessionId, picks: result.picks, runs: result.runs });
    if (!reply.ok) throw new Error('the device did not accept the result');
    return { runs: reply.runs, picks: reply.picks };
  });

  return { result, uploaded, frames: meta.frames, sampleRateHz: meta.sampleRateHz };
}
