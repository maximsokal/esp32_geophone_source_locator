import { existsSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, expect, it } from 'vitest';
import { LocalizationKernel, type KernelRecording } from '../../src/wasm/localizationKernel';

/**
 * The firmware's calculation, built for the phone (python tools/build_localization_wasm.py). This is the same synthetic
 * event as components/localization_calculate/tests/localization_calculate_run_test.c (four sensors, one source, 50 kHz):
 * the answer must match the known source to the same tolerance as the firmware's own host test.
 */
const wasmPath = resolve(__dirname, '../../../.test-build/wasm/localization.wasm');
const built = existsSync(wasmPath);

const RATE = 50000;
const PODS = [[0, 0, 0], [2, 0, 0], [0, 2, 0], [0, 0, 2]] as const;
const SOURCE = [0.25, 0.4, 0.3] as const;
const SPEED = 340;
const EMISSION_S = 0.01;
const PULSE = 20;
const NOISE_W = 50;

function recording(options: { copyChannel?: [number, number] } = {}): KernelRecording {
  let state = 0x9e3779b9;
  const noise = (amplitude: number) => {
    state ^= state << 13; state >>>= 0;
    state ^= state >>> 17;
    state ^= state << 5; state >>>= 0;
    return amplitude * ((state % 20001) / 10000 - 1);
  };
  const arrivals = PODS.map((pod) => {
    const distance = Math.hypot(pod[0] - SOURCE[0], pod[1] - SOURCE[1], pod[2] - SOURCE[2]);
    return Math.round((EMISSION_S + distance / SPEED) * RATE);
  });
  const length = Math.max(...arrivals) + PULSE + NOISE_W + 100;
  const channelUv = new Float32Array(length * 4);
  for (let channel = 0; channel < 4; channel += 1) {
    state = (0x9e3779b9 ^ ((channel * 0x85ebca6b + 1) >>> 0)) >>> 0;
    for (let i = 0; i < length; i += 1) channelUv[i * 4 + channel] = noise(0.02);
    for (let i = arrivals[channel]; i < arrivals[channel] + PULSE; i += 1) channelUv[i * 4 + channel] = 1;
  }
  if (options.copyChannel) {
    const [from, to] = options.copyChannel;
    for (let i = 0; i < length; i += 1) channelUv[i * 4 + to] = channelUv[i * 4 + from];
  }
  return {
    sampleRateHz: RATE,
    timestampsUs: Array.from({ length }, (_, i) => 5_000_000 + i * 20),
    channelUv,
    sourceIndices: Array.from({ length }, (_, i) => i),
    gapBefore: new Uint8Array(length),
  };
}

const geometryJson = JSON.stringify({
  geometry: {
    anchored: true,
    pods: PODS.map((pod) => ({
      computed: true, leveled: true, x_mm: pod[0] * 1000, y_mm: pod[1] * 1000, z_mm: pod[2] * 1000, computed_at_ms: 1000,
    })),
  },
});

const configJson = JSON.stringify({
  propagation_speed_mps: SPEED,
  propagation_speed_uncertainty_mps: 20,
  timing_uncertainty_us: 60,
  sample_rate_hz: RATE,
  noise_window_samples: NOISE_W,
  sta_window_samples: 10,
  lta_window_samples: 100,
  sta_lta_trigger_ratio: 3,
  refine_lookback_samples: 10,
  refine_lookahead_samples: 10,
  refine_amplitude_fraction: 0.5,
  snr_signal_window_samples: PULSE,
  minimum_snr: 3,
  semblance_window_half_span_samples: 10,
});

describe.skipIf(!built)('the firmware calculation in WebAssembly', () => {
  const load = () => LocalizationKernel.load(readFileSync(wasmPath));

  it('localizes the known source with the firmware code', async () => {
    const kernel = await load();
    expect(kernel.checkConfig(configJson)).toBe(0);
    const result = kernel.run({
      objectId: 'OBJ_0001', sessionId: 'S000001', recording: recording(), geometryJson, configJson, computedAtMs: 44000,
    });
    expect(result.status).toBe(0);
    expect(result.runs.length).toBe(13);
    const solved = (result.runs as Array<Record<string, any>>).filter((run) => run.solved);
    expect(solved.length).toBeGreaterThan(0);
    const tdoa = (result.runs as Array<Record<string, any>>).find((run) => run.method_name === 'tdoa_gauss_newton');
    expect(tdoa?.solved).toBe(true);
    const position = tdoa!.source_position;
    const error = Math.hypot(position.x_m - SOURCE[0], position.y_m - SOURCE[1], position.z_m - SOURCE[2]);
    expect(error).toBeLessThan(0.2);
    expect((result.picks as { channels: unknown[] }).channels).toHaveLength(4);
    expect(result.channelHealth).toEqual({ flatChannelMask: 0, identicalPairMask: 0 });
  });

  it('reports two channels carrying one signal, as the firmware does', async () => {
    const kernel = await load();
    const scaled = recording({ copyChannel: [1, 2] });
    for (let i = 0; i < scaled.channelUv.length; i += 1) scaled.channelUv[i] *= 1e6;
    const result = kernel.run({
      objectId: 'OBJ_0001', sessionId: 'S000001', recording: scaled, geometryJson, configJson, computedAtMs: 44000,
    });
    expect(result.channelHealth.identicalPairMask).toBe(1 << 3);
  });

  it('rejects malformed input instead of guessing', async () => {
    const kernel = await load();
    expect(() => kernel.run({
      objectId: 'OBJ_0001', sessionId: 'S000001',
      recording: { ...recording(), gapBefore: new Uint8Array(3) }, geometryJson, configJson, computedAtMs: 1,
    })).toThrow(/disagree/);
    expect(() => kernel.run({
      objectId: 'OBJ_0001', sessionId: 'S000001', recording: recording(), geometryJson: 'not json', configJson, computedAtMs: 1,
    })).toThrow(/geometry/);
  });
});
