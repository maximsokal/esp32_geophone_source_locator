/** Batching sender to the VS Code-launched loopback log server (/lab-log is proxied by the Tag Lab Vite server). */
export type LabEventName =
  | 'LAB_START' | 'CAMERA_PERMISSION' | 'CAMERA_READY' | 'CAMERA_FRAME' | 'CONTRAST_CANDIDATE' | 'DECODE_STARTED'
  | 'DECODE_FINISHED' | 'TAG_DETECTED' | 'TAG_POSE_ACCEPTED' | 'TAG_POSE_REJECTED' | 'TAG_CONFIRMED' | 'TAG_LOST'
  | 'FRAME_DROPPED' | 'WORKER_BUSY' | 'WORKER_ERROR' | 'READBACK_ERROR' | 'PERFORMANCE_PRESSURE' | 'SESSION_STOP' | 'LAB_ERROR';
export type LabSourceType = 'webcam' | 'image' | 'video' | 'synthetic' | 'lab';

export interface LabEvent {
  event: LabEventName;
  source: LabSourceType;
  tMs: number;
  sessionEpoch: number | null;
  frameId: number | null;
  fields: Record<string, unknown>;
}

type FetchLike = (input: string, init: { method: string; headers: Record<string, string>; body: string; keepalive?: boolean }) => Promise<unknown>;

export interface DiagnosticsOptions {
  fetchImpl?: FetchLike;
  now?: () => number;
  baseUrl?: string;
  flushIntervalMs?: number;
  maxQueue?: number;
}

export class TagLabDiagnostics {
  private queue: LabEvent[] = [];
  private active = false;
  private timer: ReturnType<typeof setInterval> | null = null;
  private readonly fetchImpl: FetchLike;
  private readonly now: () => number;
  private readonly baseUrl: string;
  private readonly flushIntervalMs: number;
  private readonly maxQueue: number;
  delivered = 0;
  failed = 0;
  dropped = 0;

  constructor(options: DiagnosticsOptions = {}) {
    this.fetchImpl = options.fetchImpl ?? ((input, init) => fetch(input, init));
    this.now = options.now ?? (() => performance.now());
    this.baseUrl = options.baseUrl ?? '/lab-log';
    this.flushIntervalMs = options.flushIntervalMs ?? 1000;
    this.maxQueue = options.maxQueue ?? 2000;
  }

  get logging(): boolean { return this.active; }

  startSession(source: LabSourceType, fields: Record<string, unknown> = {}): void {
    if (this.active) return;
    this.active = true;
    this.timer = setInterval(() => void this.flush(), this.flushIntervalMs);
    this.emit('LAB_START', source, fields);
  }

  async stopSession(source: LabSourceType, fields: Record<string, unknown> = {}): Promise<void> {
    if (!this.active) return;
    this.emit('SESSION_STOP', source, fields);
    this.active = false;
    if (this.timer !== null) { clearInterval(this.timer); this.timer = null; }
    await this.flush();
  }

  emit(event: LabEventName, source: LabSourceType, fields: Record<string, unknown> = {}, ids: { sessionEpoch?: number; frameId?: number } = {}): void {
    if (!this.active) return;
    this.queue.push({ event, source, tMs: Math.round(this.now()), sessionEpoch: ids.sessionEpoch ?? null, frameId: ids.frameId ?? null, fields });
    if (this.queue.length > this.maxQueue) {
      const over = this.queue.length - this.maxQueue;
      this.queue.splice(0, over);
      this.dropped += over;
    }
  }

  async flush(): Promise<void> {
    if (this.queue.length === 0) return;
    const batch = this.queue.splice(0, 400);
    try {
      await this.fetchImpl(`${this.baseUrl}/events`, {
        method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ events: batch }), keepalive: true,
      });
      this.delivered += batch.length;
    } catch {
      this.failed += batch.length;
    }
  }

  async pushPerformance(metrics: Record<string, unknown>): Promise<void> {
    try {
      await this.fetchImpl(`${this.baseUrl}/performance`, {
        method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(metrics), keepalive: true,
      });
    } catch { /* the page keeps working without a log receiver */ }
  }

  dispose(): void {
    if (this.timer !== null) { clearInterval(this.timer); this.timer = null; }
    this.active = false;
    this.queue = [];
  }
}
