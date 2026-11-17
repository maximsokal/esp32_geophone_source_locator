import { existsSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, expect, it } from 'vitest';
import { LocalizationKernel } from '../../src/wasm/localizationKernel';
import { parseSamplesFile } from '../../src/wasm/samplesFile';

/**
 * Files copied from the device's own SD card (board, 2026-11-16): the calculation settings the web page showed for the
 * object, and a real recording (S000022, 500 Hz, 3272 frames, three assigned REMOTE nodes, planar, remote_only).
 *
 * Why this exists: the phone path stopped with "the calculation stopped with error 258" on this very card. Every test had
 * passed, because they all used hand-made settings without a material and a hand-made 4-sensor geometry. The WebAssembly
 * module had been built with the host tests' trimmed cJSON.h in front of the real one, so strings and objects of the
 * settings were misread and any settings naming a material or a velocity range were refused. Real data found it in minutes.
 */
const wasmPath = resolve(__dirname, '../../../.test-build/wasm/localization.wasm');
const fixtures = resolve(__dirname, 'fixtures/board-2026-11-16');
const built = existsSync(wasmPath);

const settingsText = readFileSync(`${fixtures}/calc_settings.json`, 'utf-8');
const sessionJson = JSON.parse(readFileSync(`${fixtures}/S000022.session.json`, 'utf-8'));
const samples = readFileSync(`${fixtures}/S000022.samples.bin`);

async function kernel(): Promise<LocalizationKernel> {
  return LocalizationKernel.load(readFileSync(wasmPath));
}

describe.skipIf(!built)('the calculation on files from the device', () => {
  it('accepts the settings the device stores, which name a material and a velocity range', async () => {
    const settings = JSON.parse(settingsText) as Record<string, unknown>;
    expect(settings.material).toBe('concrete');
    expect(typeof settings.velocity_range).toBe('object');
    expect((await kernel()).checkConfig(settingsText)).toBe(0);
  });

  it('calculates the recording of three nodes with the device settings and writes the records back', async () => {
    const k = await kernel();
    const recording = parseSamplesFile(samples.buffer.slice(samples.byteOffset, samples.byteOffset + samples.byteLength));
    expect(recording.sampleRateHz).toBe(sessionJson.sample_rate_hz);
    expect(recording.gapBefore.length).toBe(sessionJson.samples_written);
    const result = k.run({
      objectId: 'OBJ_0001', sessionId: 'S000022', recording,
      geometryJson: JSON.stringify({ geometry: sessionJson.geometry }),
      configJson: settingsText, computedAtMs: 44_000,
    });
    expect(result.status).toBe(0);
    expect(result.runs.length).toBeGreaterThan(0);
    expect(result.picks).not.toBeNull();
    // What the device reported for this card: channels 0 and 3 flat, and, since the node of slot 1 is not assigned, a copy of
    // channel 2's signal on channel 1 that the check used to miss (pair 3 is channels 1 and 2).
    expect(result.channelHealth.flatChannelMask).toBe(0b1001);
    expect(result.channelHealth.identicalPairMask).toBe(0b001000);
  });

  it('reports a refused setting as a settings failure, not as a failed run', async () => {
    const k = await kernel();
    const recording = parseSamplesFile(samples.buffer.slice(samples.byteOffset, samples.byteOffset + samples.byteLength));
    const refused = k.run({
      objectId: 'OBJ_0001', sessionId: 'S000022', recording,
      geometryJson: JSON.stringify({ geometry: sessionJson.geometry }),
      configJson: JSON.stringify({ ...JSON.parse(settingsText), material: 'no_such_material' }), computedAtMs: 44_000,
    });
    expect(refused).toMatchObject({ status: 258, phase: 'settings', runs: [] });
  });

  it('accepts every material of the device table with its own numbers, and refuses bad ones', async () => {
    const k = await kernel();
    const table = readFileSync(resolve(__dirname, '../../../components/localization_job/localization_material_presets.c'), 'utf-8');
    const rows = [...table.matchAll(/\{\s*"([a-z_]+)",\s*(\d+)U,\s*(\d+)U,\s*([\d.]+)f,\s*([\d.]+)f,\s*([\d.]+)f,\s*([\d.]+)f\s*\}/g)];
    expect(rows.length).toBeGreaterThanOrEqual(7);
    const base = JSON.parse(settingsText) as Record<string, unknown>;
    for (const [, id, speed, sigma, vMin, vMax, ratioMin, ratioMax] of rows) {
      const settings = {
        ...base, material: id, propagation_speed_mps: Number(speed), propagation_speed_uncertainty_mps: Number(sigma),
        velocity_range: { v_min_mps: Number(vMin), v_max_mps: Number(vMax), vp_vs_min: Number(ratioMin), vp_vs_max: Number(ratioMax), source: 'preset' },
      };
      expect(k.checkConfig(JSON.stringify(settings)), `material ${id}`).toBe(0);
    }
    expect(k.checkConfig(JSON.stringify({ ...base, material: 'no_such_material' }))).toBe(258);
    expect(k.checkConfig(JSON.stringify({ ...base, material: 7 }))).toBe(258);
    expect(k.checkConfig(JSON.stringify({ ...base, velocity_range: 3 }))).toBe(258);
    // A preset material with a speed of its own is a different material: refused, as on the device.
    expect(k.checkConfig(JSON.stringify({ ...base, propagation_speed_mps: 3900 }))).toBe(258);
  });
});
