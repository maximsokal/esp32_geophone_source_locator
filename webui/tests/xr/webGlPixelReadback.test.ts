import { describe, expect, it } from 'vitest';

import { NonBlockingRgbaReadback } from '../../src/xr/webGlPixelReadback';

interface FakeBuffer { data: Uint8Array; deleted?: boolean; }
interface FakeSync { status: number; deleted?: boolean; }

function fakeWebGl2() {
  const constants = {
    PIXEL_PACK_BUFFER: 1,
    PIXEL_PACK_BUFFER_BINDING: 2,
    STREAM_READ: 3,
    RGBA: 4,
    UNSIGNED_BYTE: 5,
    SYNC_GPU_COMMANDS_COMPLETE: 6,
    TIMEOUT_EXPIRED: 7,
    WAIT_FAILED: 8,
    ALREADY_SIGNALED: 9,
    CONDITION_SATISFIED: 10,
  };
  let bound: FakeBuffer | null = null;
  let nextPixels = new Uint8Array(8);
  let throwOnReadback = false;
  const syncs: FakeSync[] = [];
  let readbackCalls = 0;
  let getParameterCalls = 0;

  const gl = {
    ...constants,
    getParameter(parameter: number) {
      if (parameter === constants.PIXEL_PACK_BUFFER_BINDING) {
        getParameterCalls += 1;
        return bound;
      }
      return null;
    },
    createBuffer(): FakeBuffer { return { data: new Uint8Array(0) }; },
    deleteBuffer(buffer: FakeBuffer) { buffer.deleted = true; },
    bindBuffer(target: number, buffer: FakeBuffer | null) {
      if (target === constants.PIXEL_PACK_BUFFER) bound = buffer;
    },
    bufferData(target: number, size: number) {
      if (target !== constants.PIXEL_PACK_BUFFER || bound === null) throw new Error('No PBO bound');
      bound.data = new Uint8Array(size);
    },
    readPixels(_x: number, _y: number, _w: number, _h: number, _format: number, _type: number, _offset: number) {
      if (bound === null) throw new Error('No PBO bound');
      bound.data.fill(0);
      bound.data.set(nextPixels.subarray(0, bound.data.length));
    },
    fenceSync(): FakeSync {
      const sync = { status: constants.TIMEOUT_EXPIRED };
      syncs.push(sync);
      return sync;
    },
    clientWaitSync(sync: FakeSync) { return sync.status; },
    deleteSync(sync: FakeSync) { sync.deleted = true; },
    getBufferSubData(target: number, offset: number, destination: Uint8Array) {
      if (target !== constants.PIXEL_PACK_BUFFER || bound === null) throw new Error('No PBO bound');
      readbackCalls += 1;
      if (throwOnReadback) throw new Error('simulated context loss');
      destination.set(bound.data.subarray(offset, offset + destination.length));
    },
  } as unknown as WebGL2RenderingContext;

  return {
    gl,
    syncs,
    setNextPixels(values: readonly number[]) { nextPixels = Uint8Array.from(values); },
    setThrowOnReadback(value: boolean) { throwOnReadback = value; },
    setExternalBinding(buffer: FakeBuffer | null) { bound = buffer; },
    signal(index: number) { syncs[index].status = constants.ALREADY_SIGNALED; },
    fail(index: number) { syncs[index].status = constants.WAIT_FAILED; },
    get readbackCalls() { return readbackCalls; },
    get getParameterCalls() { return getParameterCalls; },
    get boundBuffer() { return bound; },
  };
}

describe('NonBlockingRgbaReadback', () => {
  it('never reads CPU pixels or queries GL binding state while an old fence is unsignalled', () => {
    const fake = fakeWebGl2();
    const ring = new NonBlockingRgbaReadback<{ frame: number }>(fake.gl, 2, 1, 2);
    fake.setNextPixels([1, 2, 3, 4, 5, 6, 7, 8]);

    expect(ring.enqueue({ frame: 11 })).toBe(true);
    const queriesBeforePoll = fake.getParameterCalls;
    expect(ring.pendingCount).toBe(1);
    expect(ring.poll()).toBeNull();
    expect(fake.readbackCalls).toBe(0);
    expect(fake.getParameterCalls).toBe(queriesBeforePoll);
  });

  it('returns pixels only after the matching fence signals and preserves frame metadata', () => {
    const fake = fakeWebGl2();
    const ring = new NonBlockingRgbaReadback<{ capturedAtMs: number }>(fake.gl, 2, 1, 2);
    fake.setNextPixels([8, 7, 6, 5, 4, 3, 2, 1]);
    ring.enqueue({ capturedAtMs: 1234 });

    fake.signal(0);
    const result = ring.poll();
    expect(result).not.toBeNull();
    expect(result!.metadata.capturedAtMs).toBe(1234);
    expect(Array.from(result!.pixels)).toEqual([8, 7, 6, 5, 4, 3, 2, 1]);
    expect(fake.readbackCalls).toBe(1);
    expect(ring.pendingCount).toBe(0);
  });

  it('preserves a caller-owned PIXEL_PACK_BUFFER binding across enqueue and completed poll', () => {
    const fake = fakeWebGl2();
    const ring = new NonBlockingRgbaReadback<{ frame: number }>(fake.gl, 2, 1, 2);
    const external: FakeBuffer = { data: new Uint8Array(4) };
    fake.setExternalBinding(external);

    expect(ring.enqueue({ frame: 1 })).toBe(true);
    expect(fake.boundBuffer).toBe(external);
    fake.signal(0);
    expect(ring.poll()).not.toBeNull();
    expect(fake.boundBuffer).toBe(external);
  });

  it('drops advisory camera captures instead of blocking when every PBO is in flight', () => {
    const fake = fakeWebGl2();
    const ring = new NonBlockingRgbaReadback<{ frame: number }>(fake.gl, 2, 1, 2);
    expect(ring.enqueue({ frame: 1 })).toBe(true);
    expect(ring.enqueue({ frame: 2 })).toBe(true);
    expect(ring.pendingCount).toBe(2);

    expect(ring.enqueue({ frame: 3 })).toBe(false);
    expect(fake.readbackCalls).toBe(0);
  });

  it('releases a failed fence without attempting a blocking buffer read', () => {
    const fake = fakeWebGl2();
    const ring = new NonBlockingRgbaReadback<{ frame: number }>(fake.gl, 2, 1, 2);
    ring.enqueue({ frame: 1 });
    fake.fail(0);

    expect(ring.poll()).toBeNull();
    expect(ring.pendingCount).toBe(0);
    expect(fake.readbackCalls).toBe(0);
    expect(ring.enqueue({ frame: 2 })).toBe(true);
  });

  it('releases the slot and restores caller binding when getBufferSubData throws', () => {
    const fake = fakeWebGl2();
    const ring = new NonBlockingRgbaReadback<{ frame: number }>(fake.gl, 2, 1, 2);
    ring.enqueue({ frame: 1 });
    fake.signal(0);
    const external: FakeBuffer = { data: new Uint8Array(4) };
    fake.setExternalBinding(external);
    fake.setThrowOnReadback(true);

    expect(() => ring.poll()).toThrow('simulated context loss');
    expect(ring.pendingCount).toBe(0);
    expect(fake.boundBuffer).toBe(external);

    fake.setThrowOnReadback(false);
    expect(ring.enqueue({ frame: 2 })).toBe(true);
  });

  it('disposes pending sync objects and all PBOs without reading them back', () => {
    const fake = fakeWebGl2();
    const ring = new NonBlockingRgbaReadback<{ frame: number }>(fake.gl, 2, 1, 2);
    ring.enqueue({ frame: 1 });
    ring.dispose();
    expect(ring.pendingCount).toBe(0);
    expect(fake.readbackCalls).toBe(0);
  });
});
