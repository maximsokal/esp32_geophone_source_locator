import { expect, type Page } from '@playwright/test';

export interface GraphicsJournal {
  environment: Record<string, unknown>;
  documentControlListeners: number;
  contexts: { id: number; connected: boolean; lost: boolean; losses: number; restores: number;
    draws: number; colors: number; created: Record<string, number>; deleted: Record<string, number>;
    invalidated: Record<string, number>; live: number; listeners: number }[];
  workers: { url: string; active: boolean; posts: number; messages: number; errors: number; requests: string[]; responses: string[]; triangles: number[] }[];
}

/** Observe native allocations/events; every wrapped call still executes in Chromium. */
export function installGraphicsProbe() {
  type Gl = WebGLRenderingContext | WebGL2RenderingContext;
  const contexts: { gl: WeakRef<Gl>; stats: GraphicsJournal['contexts'][number];
    handles: WeakMap<object, string>; live: Map<string, number>; colors: Set<string>; lossExtension?: WEBGL_lose_context }[] = [];
  const workers: GraphicsJournal['workers'] = [];
  const listeners = new WeakMap<EventTarget, Set<string>>();
  const listenerIds = new WeakMap<object, number>();
  let nextListenerId = 0;
  const environment: Record<string, unknown> = { userAgent: navigator.userAgent, platform: navigator.platform,
    dpr: devicePixelRatio, hardwareConcurrency: navigator.hardwareConcurrency, viewport: [innerWidth, innerHeight],
    webgl: 'native Chromium context; SwiftShader requested by Playwright launch', physicalXR: false };

  const add = EventTarget.prototype.addEventListener;
  const remove = EventTarget.prototype.removeEventListener;
  for (const method of ['addEventListener', 'removeEventListener'] as const) {
    const original = method === 'addEventListener' ? add : remove;
    EventTarget.prototype[method] = function(type, listener, options) {
      if (listener && (this instanceof HTMLCanvasElement && /^(pointer|wheel|contextmenu)/.test(type) ||
          this === document && /^(pointermove|pointerup|keydown|keyup)$/.test(type))) {
        const callbacks = listeners.get(this) ?? new Set<string>();
        let id = listenerIds.get(listener);
        if (id === undefined) { id = ++nextListenerId; listenerIds.set(listener, id); }
        const key = `${type}:${id}:${typeof options === 'boolean' ? options : options?.capture ?? false}`;
        if (method === 'addEventListener') callbacks.add(key); else callbacks.delete(key);
        listeners.set(this, callbacks);
      }
      return original.call(this, type, listener, options);
    };
  }

  function context(gl: Gl) {
    let entry = contexts.find(item => item.gl.deref() === gl);
    if (entry) return entry;
    const stats: GraphicsJournal['contexts'][number] = { id: contexts.length, connected: false, lost: false,
      losses: 0, restores: 0, draws: 0, colors: 0, created: {}, deleted: {}, invalidated: {}, live: 0, listeners: 0 };
    entry = { gl: new WeakRef(gl), stats, handles: new WeakMap(), live: new Map(), colors: new Set() };
    contexts.push(entry);
    const debug = gl.getExtension('WEBGL_debug_renderer_info');
    Object.assign(environment, { vendor: debug ? gl.getParameter(debug.UNMASKED_VENDOR_WEBGL) : gl.getParameter(gl.VENDOR),
      renderer: debug ? gl.getParameter(debug.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER),
      version: gl.getParameter(gl.VERSION), shadingLanguage: gl.getParameter(gl.SHADING_LANGUAGE_VERSION),
      extensions: gl.getSupportedExtensions() });
    const own = entry;
    add.call(gl.canvas, 'webglcontextlost', () => {
      own.stats.losses++; own.stats.lost = true;
      for (const [kind, count] of own.live) own.stats.invalidated[kind] = (own.stats.invalidated[kind] ?? 0) + count;
      own.live.clear(); own.handles = new WeakMap();
    });
    add.call(gl.canvas, 'webglcontextrestored', () => { own.stats.restores++; own.stats.lost = false; });
    return entry;
  }

  const nativeGetContext = HTMLCanvasElement.prototype.getContext;
  HTMLCanvasElement.prototype.getContext = function(this: HTMLCanvasElement, ...args: Parameters<typeof nativeGetContext>) {
    const result = Reflect.apply(nativeGetContext, this, args);
    if (result instanceof WebGLRenderingContext || result instanceof WebGL2RenderingContext) context(result);
    return result;
  } as typeof nativeGetContext;

  const kinds = ['Buffer', 'Texture', 'Program', 'Shader', 'Framebuffer', 'Renderbuffer', 'VertexArray', 'Query', 'Sampler', 'TransformFeedback'];
  for (const prototype of [WebGLRenderingContext.prototype, WebGL2RenderingContext.prototype]) {
    for (const name of [...kinds.flatMap(kind => [`create${kind}`, `delete${kind}`]), 'fenceSync', 'deleteSync', 'drawArrays', 'drawElements', 'drawArraysInstanced', 'drawElementsInstanced']) {
      const descriptor = Object.getOwnPropertyDescriptor(prototype, name);
      if (typeof descriptor?.value !== 'function') continue;
      const native = descriptor.value;
      Object.defineProperty(prototype, name, { ...descriptor, value: function(this: Gl, ...args: unknown[]) {
        const result = Reflect.apply(native, this, args);
        const entry = context(this);
        if (name.startsWith('create') || name === 'fenceSync') {
          const kind = name === 'fenceSync' ? 'Sync' : name.slice(6);
          if (result) { entry.handles.set(result, kind); entry.live.set(kind, (entry.live.get(kind) ?? 0) + 1);
            entry.stats.created[kind] = (entry.stats.created[kind] ?? 0) + 1; }
        } else if (name.startsWith('delete')) {
          const handle = args[0];
          if (handle && typeof handle === 'object') {
            const kind = entry.handles.get(handle);
            if (kind) { entry.handles.delete(handle); entry.live.set(kind, entry.live.get(kind)! - 1);
              entry.stats.deleted[kind] = (entry.stats.deleted[kind] ?? 0) + 1; }
          }
        } else {
          entry.stats.draws++;
          // Read actual framebuffer pixels while the browser's drawing buffer is valid.
          if (entry.stats.draws % 60 === 1 && this.getParameter(this.FRAMEBUFFER_BINDING) === null && !this.isContextLost()) {
            const width = Math.min(64, this.drawingBufferWidth), height = Math.min(64, this.drawingBufferHeight);
            const pixels = new Uint8Array(width * height * 4);
            this.readPixels(Math.floor((this.drawingBufferWidth - width) / 2), Math.floor((this.drawingBufferHeight - height) / 2), width, height, this.RGBA, this.UNSIGNED_BYTE, pixels);
            for (let offset = 0; offset < pixels.length && entry.colors.size < 256; offset += 4) entry.colors.add(pixels.subarray(offset, offset + 4).join(','));
            entry.stats.colors = entry.colors.size;
          }
        }
        return result;
      } });
    }
  }

  const NativeWorker = window.Worker;
  window.Worker = new Proxy(NativeWorker, { construct(target, args: [string | URL, WorkerOptions?]) {
    const worker = Reflect.construct(target, args) as Worker;
    const entry = { url: String(args[0]), active: true, posts: 0, messages: 0, errors: 0, requests: [] as string[], responses: [] as string[], triangles: [] as number[] };
    workers.push(entry);
    add.call(worker, 'message', (event: Event) => {
      const data = (event as MessageEvent).data;
      entry.messages++; entry.responses.push(String(data?.type));
      if (typeof data?.result?.triangleCount === 'number') entry.triangles.push(data.result.triangleCount);
    });
    add.call(worker, 'error', () => { entry.errors++; });
    const post = worker.postMessage.bind(worker);
    worker.postMessage = ((...messageArgs: unknown[]) => {
      entry.posts++; entry.requests.push(String((messageArgs[0] as { type?: unknown })?.type));
      return Reflect.apply(post, worker, messageArgs);
    }) as Worker['postMessage'];
    const terminate = worker.terminate.bind(worker);
    worker.terminate = () => { entry.active = false; terminate(); };
    return worker;
  } });

  window.__graphics = {
    snapshot: () => ({ environment, documentControlListeners: listeners.get(document)?.size ?? 0,
      workers: workers.map(worker => ({ ...worker, url: worker.url.startsWith('blob:') ? 'blob:emitted-inline-worker' : worker.url })),
      contexts: contexts.map(({ gl: weak, stats, live }) => {
        const gl = weak.deref();
        return { ...stats, connected: gl?.canvas instanceof HTMLCanvasElement && gl.canvas.isConnected,
          live: [...live.values()].reduce((sum, count) => sum + count, 0), listeners: gl ? listeners.get(gl.canvas)?.size ?? 0 : 0 };
      }) }),
    lose: (id, restore) => {
      const gl = contexts[id]?.gl.deref();
      const entry = contexts[id];
      const extension = entry.lossExtension ?? gl?.getExtension('WEBGL_lose_context');
      if (!extension) throw new Error(`Native context ${id} cannot be lost/restored`);
      entry.lossExtension = extension;
      if (restore) extension.restoreContext(); else extension.loseContext();
    },
  };
}

declare global {
  interface Window { __graphics: { snapshot: () => GraphicsJournal; lose: (id: number, restore: boolean) => void } }
}

export const journal = (page: Page) => page.evaluate(() => window.__graphics.snapshot());
export function assertReleased(value: GraphicsJournal) {
  expect(value.documentControlListeners, 'Document pointer/keyboard control listeners released').toBe(0);
  expect(value.contexts.filter(context => context.live > 0 || context.listeners > 0), 'Native GPU handles and canvas controls released or invalidated by context loss').toEqual([]);
  expect(value.workers.filter(worker => worker.active), 'All emitted Workers terminated').toEqual([]);
}
export async function released(page: Page) {
  await expect.poll(async () => {
    const value = await journal(page);
    return value.documentControlListeners + value.contexts.reduce((sum, context) => sum + context.live + context.listeners, 0) + value.workers.filter(worker => worker.active).length;
  }, { timeout: 12_000, message: 'Deferred R3F disposal and production Worker teardown' }).toBe(0);
  assertReleased(await journal(page));
}
export async function rendered(page: Page) {
  await expect(page.locator('.workspace canvas').first()).toBeVisible();
  await expect.poll(async () => (await journal(page)).contexts.filter(context => context.connected && context.draws > 0 && context.colors > 1).length,
    { message: 'Real draw calls and nonuniform native framebuffer pixels' }).toBeGreaterThan(0);
}
