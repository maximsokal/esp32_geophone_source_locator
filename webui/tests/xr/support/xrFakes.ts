export interface Deferred<T> {
  promise: Promise<T>;
  resolve: (value: T) => void;
  reject: (reason?: unknown) => void;
}

export function deferred<T>(): Deferred<T> {
  let resolve!: (value: T) => void;
  let reject!: (reason?: unknown) => void;
  const promise = new Promise<T>((resolvePromise, rejectPromise) => {
    resolve = resolvePromise;
    reject = rejectPromise;
  });
  return { promise, resolve, reject };
}

type Listener = EventListenerOrEventListenerObject;

export class FakeXrSession {
  active = true;
  endCalls = 0;
  endFailures = 0;
  updateCalls = 0;
  referenceSpaceRequests: string[] = [];
  lightProbeRequests = 0;
  rafRequests = 0;
  rafCancellations = 0;
  updateRenderStatePromise: Promise<void> | null = null;
  lightProbePromise: Promise<object> | null = null;
  readonly referenceSpacePromises = new Map<string, Promise<object>>();
  readonly listeners = new Map<string, Set<Listener>>();
  readonly callbacks = new Map<number, (time: number, frame: never) => void>();
  private nextFrameId = 0;
  readonly domOverlayState = { type: 'screen' };
  environmentBlendMode = 'opaque';
  visibilityState: 'visible' | 'visible-blurred' | 'hidden' = 'visible';

  addEventListener(type: string, listener: Listener | null): void {
    if (listener === null) return;
    const list = this.listeners.get(type) ?? new Set<Listener>();
    list.add(listener);
    this.listeners.set(type, list);
  }

  removeEventListener(type: string, listener: Listener | null): void {
    if (listener === null) return;
    this.listeners.get(type)?.delete(listener);
  }

  dispatchEvent(event: Event): boolean {
    for (const listener of this.listeners.get(event.type) ?? []) {
      if (typeof listener === 'function') listener.call(this, event);
      else listener.handleEvent(event);
    }
    return true;
  }

  listenerCount(type: string): number {
    return this.listeners.get(type)?.size ?? 0;
  }

  updateRenderState(): Promise<void> {
    this.updateCalls += 1;
    return this.updateRenderStatePromise ?? Promise.resolve();
  }

  requestReferenceSpace(type: string): Promise<object> {
    this.referenceSpaceRequests.push(type);
    return this.referenceSpacePromises.get(type) ?? Promise.resolve({});
  }

  requestLightProbe(): Promise<object> {
    this.lightProbeRequests += 1;
    return this.lightProbePromise ?? Promise.resolve({});
  }

  requestAnimationFrame(callback: (time: number, frame: never) => void): number {
    const id = ++this.nextFrameId;
    this.rafRequests += 1;
    if (this.active) this.callbacks.set(id, callback);
    return id;
  }

  cancelAnimationFrame(id: number): void {
    this.rafCancellations += 1;
    this.callbacks.delete(id);
  }

  get liveFrameCallbacks(): number {
    return this.callbacks.size;
  }

  end(): Promise<void> {
    this.endCalls += 1;
    if (this.endFailures > 0) {
      this.endFailures -= 1;
      return Promise.reject(new Error('Fake XR session end failed.'));
    }
    this.finish();
    return Promise.resolve();
  }

  finish(): void {
    if (!this.active) return;
    this.active = false;
    this.callbacks.clear();
    this.dispatchEvent(new Event('end'));
  }

  interrupt(): void {
    this.finish();
  }

  emitVisibilityChange(): void {
    this.dispatchEvent(new Event('visibilitychange'));
  }
}

export class FakeXrSystem {
  requests = 0;
  readonly activeSessions = new Set<FakeXrSession>();
  private readonly queued: Promise<FakeXrSession>[] = [];

  isSessionSupported(): Promise<boolean> {
    return Promise.resolve(true);
  }

  requestSession(): Promise<FakeXrSession> {
    this.requests += 1;
    const next = this.queued.shift();
    if (next === undefined) return Promise.reject(new Error('No fake XR session queued.'));
    return next.then((session) => {
      this.activeSessions.add(session);
      return session;
    });
  }

  queue(session: FakeXrSession): void {
    this.queued.push(Promise.resolve(session));
  }

  queueDeferred(): Deferred<FakeXrSession> {
    const pending = deferred<FakeXrSession>();
    this.queued.push(pending.promise);
    return pending;
  }

  get liveSessions(): number {
    return [...this.activeSessions].filter((session) => session.active).length;
  }
}

export class FakeWakeLockSentinel extends EventTarget {
  released = false;
  releaseCalls = 0;

  constructor(private readonly didRelease: () => void) {
    super();
  }

  async release(): Promise<void> {
    if (this.released) return;
    this.released = true;
    this.releaseCalls += 1;
    this.didRelease();
    this.dispatchEvent(new Event('release'));
  }
}

export class FakeWakeLock {
  requests = 0;
  readonly liveSentinels = new Set<FakeWakeLockSentinel>();
  readonly sentinels: FakeWakeLockSentinel[] = [];
  private readonly queued: Promise<FakeWakeLockSentinel>[] = [];

  request(): Promise<FakeWakeLockSentinel> {
    this.requests += 1;
    const next = this.queued.shift();
    return next ?? Promise.resolve(this.createSentinel());
  }

  deferNext(): Deferred<FakeWakeLockSentinel> {
    const pending = deferred<FakeWakeLockSentinel>();
    this.queued.push(pending.promise);
    return pending;
  }

  createSentinel(): FakeWakeLockSentinel {
    const sentinel = new FakeWakeLockSentinel(() => this.liveSentinels.delete(sentinel));
    this.sentinels.push(sentinel);
    this.liveSentinels.add(sentinel);
    return sentinel;
  }
}

export class FakeXrLayer {
  framebuffer = null;
  framebufferWidth = 64;
  framebufferHeight = 64;

  constructor(_session: unknown, _gl: unknown) {}

  getViewport(): { x: number; y: number; width: number; height: number } {
    return { x: 0, y: 0, width: 64, height: 64 };
  }
}
