import { describe, expect, it } from 'vitest';
import { parseSamplesFile, SamplesFileError } from '../../src/wasm/samplesFile';
import { v2File } from './samplesFixture';

describe('samples.bin reader', () => {
  it('decodes v2 blocks the way the firmware reader does', () => {
    const file = v2File({
      rate: 4000,
      scale: [0.5, 2, 1, 152.587890625],
      offset: [10, 0, -3, 0],
      blocks: [
        { firstIndex: 100, firstTimestampUs: 5_000_000, codes: [[2, 3, 4, 5], [-2, -3, -4, -5], [32767, -32768, 0, 1]] },
        // 2 conversions were lost: the source index jumps by 2 and the block carries the flag.
        { firstIndex: 105, firstTimestampUs: 5_001_250, gap: true, codes: [[1, 1, 1, 1], [2, 2, 2, 2]] },
      ],
    });
    const recording = parseSamplesFile(file);
    expect(recording.sampleRateHz).toBe(4000);
    expect(recording.gapBefore).toEqual(new Uint8Array([0, 0, 0, 1, 0]));
    expect(Array.from(recording.sourceIndices, Number)).toEqual([100, 101, 102, 105, 106]);
    expect(Array.from(recording.timestampsUs, Number)).toEqual([5_000_000, 5_000_250, 5_000_500, 5_001_250, 5_001_500]);
    // value = code * scale + offset, in float32
    expect(Array.from(recording.channelUv.slice(0, 4))).toEqual([11, 6, -3 + 4, 5 * Math.fround(152.587890625)].map(Math.fround));
    expect(recording.channelUv[8]).toBe(Math.fround(32767 * 0.5 + 10));
    expect(recording.channelUv[9]).toBe(-65536);
  });

  it('reads v1 frames and gives them consecutive indices', () => {
    const bytes = new ArrayBuffer(24 + 2 * 24);
    const view = new DataView(bytes);
    'GEOSEIS1'.split('').forEach((character, i) => view.setUint8(i, character.charCodeAt(0)));
    view.setUint16(8, 1, true);
    view.setUint16(10, 4, true);
    view.setUint32(12, 500, true);
    view.setUint32(16, 24, true);
    for (let frame = 0; frame < 2; frame += 1) {
      const at = 24 + frame * 24;
      view.setBigInt64(at, BigInt(1_000_000 + frame * 2000), true);
      for (let channel = 0; channel < 4; channel += 1) view.setFloat32(at + 8 + channel * 4, frame * 10 + channel + 0.5, true);
    }
    const recording = parseSamplesFile(bytes);
    expect(Array.from(recording.timestampsUs, Number)).toEqual([1_000_000, 1_002_000]);
    expect(Array.from(recording.channelUv)).toEqual([0.5, 1.5, 2.5, 3.5, 10.5, 11.5, 12.5, 13.5]);
    expect(Array.from(recording.sourceIndices, Number)).toEqual([0, 1]);
    expect(recording.gapBefore).toEqual(new Uint8Array([0, 0]));
  });

  it('drops a torn last block and refuses a corrupt file instead of guessing', () => {
    const whole = v2File({
      rate: 500,
      blocks: [
        { firstIndex: 0, firstTimestampUs: 0, codes: [[1, 2, 3, 4], [5, 6, 7, 8]] },
        { firstIndex: 2, firstTimestampUs: 4000, codes: [[9, 9, 9, 9], [9, 9, 9, 9]] },
      ],
    });
    const torn = whole.slice(0, whole.byteLength - 5);
    expect(parseSamplesFile(torn).gapBefore.length).toBe(2);

    const wrongMagic = new Uint8Array(whole.slice(0));
    wrongMagic[0] = 0x58;
    expect(() => parseSamplesFile(wrongMagic.buffer)).toThrow(SamplesFileError);
    expect(() => parseSamplesFile(new ArrayBuffer(10))).toThrow(/shorter/);

    const unknownFlag = new DataView(whole.slice(0));
    unknownFlag.setUint16(68 + 18, 2, true);
    expect(() => parseSamplesFile(unknownFlag.buffer)).toThrow(/flags/);

    const holeWithoutFlag = v2File({
      rate: 500,
      blocks: [
        { firstIndex: 0, firstTimestampUs: 0, codes: [[1, 1, 1, 1]] },
        { firstIndex: 5, firstTimestampUs: 10_000, codes: [[1, 1, 1, 1]] },
      ],
    });
    expect(() => parseSamplesFile(holeWithoutFlag)).toThrow(/hole/);

    const backwards = v2File({
      rate: 500,
      blocks: [
        { firstIndex: 10, firstTimestampUs: 0, codes: [[1, 1, 1, 1]] },
        { firstIndex: 4, firstTimestampUs: 10_000, codes: [[1, 1, 1, 1]] },
      ],
    });
    expect(() => parseSamplesFile(backwards)).toThrow(/backwards/);

    const badScale = v2File({ rate: 500, scale: [0, 1, 1, 1], blocks: [{ firstIndex: 0, firstTimestampUs: 0, codes: [[1, 1, 1, 1]] }] });
    expect(() => parseSamplesFile(badScale)).toThrow(/scale/);
  });
});
