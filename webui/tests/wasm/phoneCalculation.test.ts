import { existsSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, expect, it, vi } from 'vitest';
import { LocalizationKernel } from '../../src/wasm/localizationKernel';
import { calculateOnPhone, PhoneCalculationError, type KernelRunner } from '../../src/wasm/phoneCalculation';
import { v2File } from './samplesFixture';

/**
 * The whole phone path with the device replaced by fakes: meta, settings and the recording come in, the firmware's own
 * calculation (WebAssembly) runs, and what goes back is what the device would store. The recording is a v2 samples.bin of
 * the same synthetic event the firmware's host test localizes (four sensors, one source, 50 kHz).
 */
const wasmPath = resolve(__dirname, '../../../.test-build/wasm/localization.wasm');
const built = existsSync(wasmPath);

const RATE = 50000;
const PODS = [[0, 0, 0], [2, 0, 0], [0, 2, 0], [0, 0, 2]] as const;
const SOURCE = [0.25, 0.4, 0.3] as const;
const SPEED = 340;
const SCALE_UV_PER_LSB = 0.001; // the synthetic pulse of 1 uV is 1000 codes
const PULSE = 20;

function eventCodes(): number[][] {
  let state = 0x9e3779b9;
  const noise = () => {
    state ^= state << 13; state >>>= 0;
    state ^= state >>> 17;
    state ^= state << 5; state >>>= 0;
    return 0.02 * ((state % 20001) / 10000 - 1);
  };
  const arrivals = PODS.map((pod) => Math.round((0.01 + Math.hypot(pod[0] - SOURCE[0], pod[1] - SOURCE[1], pod[2] - SOURCE[2]) / SPEED) * RATE));
  const length = Math.max(...arrivals) + PULSE + 50 + 100;
  const frames: number[][] = Array.from({ length }, () => [0, 0, 0, 0]);
  for (let channel = 0; channel < 4; channel += 1) {
    for (let i = 0; i < length; i += 1) frames[i][channel] = Math.round(noise() / SCALE_UV_PER_LSB);
    for (let i = arrivals[channel]; i < arrivals[channel] + PULSE; i += 1) frames[i][channel] = Math.round(1 / SCALE_UV_PER_LSB);
  }
  return frames;
}

const settings = {
  propagation_speed_mps: SPEED, propagation_speed_uncertainty_mps: 20, timing_uncertainty_us: 60, sample_rate_hz: RATE,
  noise_window_samples: 50, sta_window_samples: 10, lta_window_samples: 100, sta_lta_trigger_ratio: 3,
  refine_lookback_samples: 10, refine_lookahead_samples: 10, refine_amplitude_fraction: 0.5, snr_signal_window_samples: PULSE,
  minimum_snr: 3, semblance_window_half_span_samples: 10,
};
const geometry = {
  geometry: {
    anchored: true,
    pods: PODS.map((pod) => ({ computed: true, leveled: true, x_mm: pod[0] * 1000, y_mm: pod[1] * 1000, z_mm: pod[2] * 1000, computed_at_ms: 1000 })),
  },
};

function fakeDevice(overrides: { frames?: number; rate?: number; settings?: unknown | null } = {}) {
  const codes = eventCodes();
  const file = v2File({
    rate: RATE,
    scale: [SCALE_UV_PER_LSB, SCALE_UV_PER_LSB, SCALE_UV_PER_LSB, SCALE_UV_PER_LSB],
    blocks: Array.from({ length: Math.ceil(codes.length / 256) }, (_, block) => ({
      firstIndex: block * 256,
      firstTimestampUs: 5_000_000 + Math.floor((block * 256 * 1_000_000) / RATE),
      codes: codes.slice(block * 256, block * 256 + 256),
    })),
  });
  const upload = vi.fn(async (body: { runs: unknown[]; picks: unknown | null }) => ({ ok: true, runs: body.runs.length, picks: body.picks !== null }));
  return {
    codesLength: codes.length,
    upload,
    deps: {
      fetchMeta: vi.fn(async (objectId: string, sessionId: string) => ({
        objectId, sessionId, sampleRateHz: overrides.rate ?? RATE, frames: overrides.frames ?? codes.length,
        formatVersion: 2, geometrySource: 'channel_sounding' as const, geometry,
      })),
      fetchSettings: vi.fn(async () => ({
        settings: overrides.settings === undefined ? settings : overrides.settings,
        presets: [], standardProcessing: {},
      })) as never,
      download: vi.fn(async () => file),
      upload,
      now: () => 44_000,
    },
  };
}

describe.skipIf(!built)('calculating a finished session on the phone', () => {
  async function inPlaceRunner(): Promise<KernelRunner> {
    const kernel = await LocalizationKernel.load(readFileSync(wasmPath));
    return async (input) => kernel.run(input);
  }

  it('downloads, calculates with the firmware code and uploads what the device would store', async () => {
    const device = fakeDevice();
    const stages: string[] = [];
    const outcome = await calculateOnPhone('OBJ_0001', 'S000001', await inPlaceRunner(), {
      deps: device.deps as never, onStage: (stage) => stages.push(stage),
    });
    expect(stages).toEqual(['meta', 'settings', 'download', 'calculate', 'upload']);
    expect(outcome.frames).toBe(device.codesLength);
    const tdoa = (outcome.result.runs as Array<Record<string, any>>).find((run) => run.method_name === 'tdoa_gauss_newton');
    expect(tdoa?.solved).toBe(true);
    const position = tdoa!.source_position;
    expect(Math.hypot(position.x_m - SOURCE[0], position.y_m - SOURCE[1], position.z_m - SOURCE[2])).toBeLessThan(0.25);
    // What goes to the device is exactly what the kernel produced, for this object and session.
    expect(device.upload).toHaveBeenCalledTimes(1);
    const sent = device.upload.mock.calls[0][0];
    expect(sent).toMatchObject({ objectId: 'OBJ_0001', sessionId: 'S000001' });
    expect(sent.runs).toEqual(outcome.result.runs);
    expect((sent.runs as Array<Record<string, any>>).every((run) => run.object_id === 'OBJ_0001' && run.session_id === 'S000001')).toBe(true);
    expect(outcome.uploaded.runs).toBe(outcome.result.runs.length);
  });

  it('stops at the stage that failed and uploads nothing', async () => {
    const runner = await inPlaceRunner();

    const missing = fakeDevice({ settings: null });
    await expect(calculateOnPhone('OBJ_0001', 'S000001', runner, { deps: missing.deps as never }))
      .rejects.toMatchObject({ stage: 'settings' });

    const shortFile = fakeDevice({ frames: 123 });
    await expect(calculateOnPhone('OBJ_0001', 'S000001', runner, { deps: shortFile.deps as never }))
      .rejects.toMatchObject({ stage: 'download', message: expect.stringContaining('123') });

    const wrongRate = fakeDevice({ rate: 4000 });
    await expect(calculateOnPhone('OBJ_0001', 'S000001', runner, { deps: wrongRate.deps as never }))
      .rejects.toMatchObject({ stage: 'download' });

    const rejectedRun: KernelRunner = async () => ({ status: 258, picks: null, runs: [], channelHealth: { flatChannelMask: 0, identicalPairMask: 0 } });
    const failing = fakeDevice();
    await expect(calculateOnPhone('OBJ_0001', 'S000001', rejectedRun, { deps: failing.deps as never }))
      .rejects.toThrow(PhoneCalculationError);

    for (const device of [missing, shortFile, wrongRate, failing]) expect(device.upload).not.toHaveBeenCalled();
  });

  it('reports a device that refuses the result', async () => {
    const device = fakeDevice();
    device.deps.upload = vi.fn(async () => ({ ok: false, runs: 0, picks: false }));
    await expect(calculateOnPhone('OBJ_0001', 'S000001', await inPlaceRunner(), { deps: device.deps as never }))
      .rejects.toMatchObject({ stage: 'upload' });
  });
});
