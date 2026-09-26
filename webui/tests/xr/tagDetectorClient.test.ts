import { describe, expect, it } from 'vitest';
import { TagDetectorClient, type TagCameraFrame } from '../../src/xr/fiducial/tagDetectorClient';

class FakeWorker {
  messages: { message: any; transfer?: Transferable[] }[] = [];
  terminated = false;
  private listeners = new Map<string, Set<(event: any) => void>>();

  addEventListener(type: string, listener: (event: any) => void): void {
    const listeners = this.listeners.get(type) ?? new Set(); listeners.add(listener); this.listeners.set(type, listeners);
  }
  removeEventListener(type: string, listener: (event: any) => void): void { this.listeners.get(type)?.delete(listener); }
  postMessage(message: unknown, transfer?: Transferable[]): void { this.messages.push({ message, transfer }); }
  terminate(): void { this.terminated = true; }
  emit(type: string, data: unknown): void { for (const listener of this.listeners.get(type) ?? []) listener({ data, message: 'fake failure' }); }
}

function cameraFrame(): TagCameraFrame {
  return {
    width: 960, height: 540, rgba: new Uint8Array(960 * 540 * 4), capturedAtMs: 1234,
    projectionMatrix: new Float32Array(16).fill(1), worldFromView: new Float32Array(16).fill(2),
  };
}

describe('tag detector worker client', () => {
  it('sends the inline worker protocol and returns frame matrices with detections', async () => {
    const worker = new FakeWorker(); const client = new TagDetectorClient(() => worker as never);
    expect(worker.messages[0].message).toEqual({ type: 'reset', epoch: 1 });
    const frame = cameraFrame(); const resultPromise = client.detect(frame);
    const request = worker.messages[1].message;
    expect({
      type: request.type, requestId: request.requestId, epoch: request.epoch,
      width: request.width, height: request.height, capturedAtMs: request.capturedAtMs,
    }).toEqual({ type: 'detect', requestId: 1, epoch: 1, width: 960, height: 540, capturedAtMs: 1234 });
    expect(request.rgba !== frame.rgba).toBe(true);
    const detection = { tagId: 0, corners: [[0.1, 0.2], [0.8, 0.2], [0.8, 0.9], [0.1, 0.9]], hammingDistance: 0 };
    worker.emit('message', { type: 'result', requestId: request.requestId, epoch: 1, detections: [detection] });
    const result = await resultPromise;
    expect(result).toMatchObject({ capturedAtMs: 1234, detections: [detection] });
    expect(Array.from(result?.projectionMatrix ?? [])).toEqual(Array.from(frame.projectionMatrix));
    expect(Array.from(result?.worldFromView ?? [])).toEqual(Array.from(frame.worldFromView));
    client.dispose();
    expect(worker.terminated).toBe(true);
  });

  it('drops frames while a worker request is in flight', async () => {
    const worker = new FakeWorker(); const client = new TagDetectorClient(() => worker as never);
    const pending = client.detect(cameraFrame());
    await expect(client.detect(cameraFrame())).resolves.toBeNull();
    expect(worker.messages.filter((item) => item.message.type === 'detect')).toHaveLength(1);
    const request = worker.messages[1].message;
    worker.emit('message', { type: 'result', requestId: request.requestId, epoch: request.epoch, detections: [] });
    await expect(pending).resolves.toMatchObject({ detections: [] });
    client.dispose();
  });

  it('rejects pending work when the epoch resets and ignores its stale response', async () => {
    const worker = new FakeWorker(); const client = new TagDetectorClient(() => worker as never);
    const pending = client.detect(cameraFrame()); const request = worker.messages[1].message;
    client.reset();
    await expect(pending).rejects.toThrow(/obsolete XR session|reset/);
    expect(worker.messages[2].message).toEqual({ type: 'reset', epoch: 2 });
    worker.emit('message', { type: 'result', requestId: request.requestId, epoch: 1, detections: [] });
    expect(client.busy).toBe(false);
    client.dispose();
  });
});
