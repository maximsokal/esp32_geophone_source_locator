import type { KernelRecording } from './localizationKernel';

/**
 * samples.bin as the recorder writes it (components/seismic_recorder/seismic_recorder_internal.h, GEOSEIS1):
 *   24-byte header: "GEOSEIS1", u16 version, u16 channels, u32 sample rate, u32 frame size, u32 reserved.
 *   v1: native frames, i64 timestamp_us and four float32 microvolts each.
 *   v2: +44 bytes (four float32 scales in uV/LSB, four float32 offsets in uV, float32 range, u32 oversampling, u32 SPI clock),
 *       then blocks of a 20-byte header (u64 first source index, u64 first timestamp_us, u16 frames, u16 flags; bit 0: a hole
 *       precedes the block) and `frames` groups of four int16 codes. Everything little-endian.
 * The frames come out exactly as the firmware's reader makes them: value = code * scale + offset in float32, timestamp =
 * block start + offset * 1e6 / rate in whole microseconds.
 */
const MAGIC = 'GEOSEIS1';
const HEADER_BYTES = 24;
const V2_METADATA_BYTES = 44;
const V2_BLOCK_HEADER_BYTES = 20;
const V2_BLOCK_MAX_FRAMES = 256;
const FLAG_GAP_BEFORE = 1;
const CHANNELS = 4;

export class SamplesFileError extends Error {}

function fail(reason: string): never {
  throw new SamplesFileError(`samples.bin: ${reason}`);
}

export function parseSamplesFile(bytes: ArrayBuffer): KernelRecording {
  if (bytes.byteLength < HEADER_BYTES) fail('shorter than its header');
  const view = new DataView(bytes);
  let magic = '';
  for (let i = 0; i < 8; i += 1) magic += String.fromCharCode(view.getUint8(i));
  if (magic !== MAGIC) fail('not a GEOSEIS1 file');
  const version = view.getUint16(8, true);
  const channels = view.getUint16(10, true);
  const sampleRateHz = view.getUint32(12, true);
  if (channels !== CHANNELS) fail(`${channels} channels, expected ${CHANNELS}`);
  if (sampleRateHz === 0) fail('sample rate is zero');
  if (version === 1) return parseV1(view, sampleRateHz);
  if (version === 2) return parseV2(view, sampleRateHz);
  return fail(`unknown version ${version}`);
}

function parseV1(view: DataView, sampleRateHz: number): KernelRecording {
  const frameBytes = 8 + CHANNELS * 4;
  const count = Math.floor((view.byteLength - HEADER_BYTES) / frameBytes);
  if (count === 0) fail('no frames');
  const timestampsUs = new Array<number>(count);
  const channelUv = new Float32Array(count * CHANNELS);
  for (let i = 0; i < count; i += 1) {
    const frame = HEADER_BYTES + i * frameBytes;
    timestampsUs[i] = Number(view.getBigInt64(frame, true));
    for (let channel = 0; channel < CHANNELS; channel += 1) {
      channelUv[i * CHANNELS + channel] = view.getFloat32(frame + 8 + channel * 4, true);
    }
  }
  // A v1 file has no source indices: every frame follows the one before it.
  return {
    sampleRateHz,
    timestampsUs,
    channelUv,
    sourceIndices: Array.from({ length: count }, (_, i) => i),
    gapBefore: new Uint8Array(count),
  };
}

function parseV2(view: DataView, sampleRateHz: number): KernelRecording {
  if (view.byteLength < HEADER_BYTES + V2_METADATA_BYTES) fail('shorter than its v2 header');
  const scale: number[] = [];
  const offset: number[] = [];
  for (let channel = 0; channel < CHANNELS; channel += 1) {
    scale.push(view.getFloat32(HEADER_BYTES + channel * 4, true));
    offset.push(view.getFloat32(HEADER_BYTES + 16 + channel * 4, true));
  }
  if (!scale.every((value) => Number.isFinite(value) && value > 0) || !offset.every(Number.isFinite)) {
    fail('scale or offset is not finite');
  }

  // Walk the blocks first to size the arrays; a partly written last block is dropped, as the firmware drops a torn tail.
  const blocks: Array<{ at: number; frames: number; firstIndex: bigint; firstTimestamp: bigint; gap: boolean }> = [];
  let position = HEADER_BYTES + V2_METADATA_BYTES;
  let total = 0;
  while (position + V2_BLOCK_HEADER_BYTES <= view.byteLength) {
    const frames = view.getUint16(position + 16, true);
    const flags = view.getUint16(position + 18, true);
    if (frames === 0 || frames > V2_BLOCK_MAX_FRAMES) fail(`block of ${frames} frames at byte ${position}`);
    if ((flags & ~FLAG_GAP_BEFORE) !== 0) fail(`unknown block flags ${flags} at byte ${position}`);
    const end = position + V2_BLOCK_HEADER_BYTES + frames * CHANNELS * 2;
    if (end > view.byteLength) break;
    blocks.push({
      at: position + V2_BLOCK_HEADER_BYTES,
      frames,
      firstIndex: view.getBigUint64(position, true),
      firstTimestamp: view.getBigUint64(position + 8, true),
      gap: (flags & FLAG_GAP_BEFORE) !== 0,
    });
    total += frames;
    position = end;
  }
  if (total === 0) fail('no frames');

  const timestampsUs = new Array<number>(total);
  const sourceIndices = new Array<bigint>(total);
  const channelUv = new Float32Array(total * CHANNELS);
  const gapBefore = new Uint8Array(total);
  const rate = BigInt(sampleRateHz);
  let frameIndex = 0;
  let expectedIndex: bigint | null = null;
  for (const block of blocks) {
    if (expectedIndex !== null && block.firstIndex < expectedIndex) fail('source indices go backwards');
    if (expectedIndex !== null && block.firstIndex > expectedIndex && !block.gap) fail('a hole without its flag');
    if (block.gap) gapBefore[frameIndex] = 1;
    for (let i = 0; i < block.frames; i += 1) {
      timestampsUs[frameIndex] = Number(block.firstTimestamp + (BigInt(i) * 1_000_000n) / rate);
      sourceIndices[frameIndex] = block.firstIndex + BigInt(i);
      for (let channel = 0; channel < CHANNELS; channel += 1) {
        const code = view.getInt16(block.at + (i * CHANNELS + channel) * 2, true);
        channelUv[frameIndex * CHANNELS + channel] = Math.fround(Math.fround(code * scale[channel]) + offset[channel]);
      }
      frameIndex += 1;
    }
    expectedIndex = block.firstIndex + BigInt(block.frames);
  }
  return { sampleRateHz, timestampsUs, channelUv, sourceIndices, gapBefore };
}
