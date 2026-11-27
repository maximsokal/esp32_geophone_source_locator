/**
 * Aggregated live-mesh statistics (TASK 11): one XR_LIVE_MESH_BUILD_STATS and one XR_LIVE_MESH_RENDER_STATS record per reporting interval instead of a record
 * per build or per frame. Samples are kept in bounded rings, quantiles are taken at flush time, and nothing is emitted when nothing happened in the interval.
 * Pure: the clock and the sink are injected.
 */
export const LIVE_MESH_STATS_INTERVAL_MS = 10_000;
const MAX_SAMPLES = 512;

export interface LiveMeshStatsEvent { event: 'XR_LIVE_MESH_BUILD_STATS' | 'XR_LIVE_MESH_RENDER_STATS'; fields: Record<string, number | string> }

function quantile(sorted: number[], q: number): number { return sorted.length === 0 ? 0 : sorted[Math.min(sorted.length - 1, Math.max(0, Math.ceil(q * sorted.length) - 1))]; }
const r1 = (value: number): number => Number(value.toFixed(1));

class Ring {
  private values: number[] = [];
  push(value: number): void { this.values.push(value); if (this.values.length > MAX_SAMPLES) this.values.shift(); }
  get size(): number { return this.values.length; }
  stats(): { p50: number; p95: number; max: number } {
    const sorted = this.values.slice().sort((a, b) => a - b);
    return { p50: r1(quantile(sorted, 0.5)), p95: r1(quantile(sorted, 0.95)), max: r1(sorted.at(-1) ?? 0) };
  }
  clear(): void { this.values = []; }
}

export class LiveMeshStats {
  private readonly buildMs = new Ring();
  private readonly renderMs = new Ring();
  private readonly uploadMs = new Ring();
  private builds = 0;
  private frames = 0;
  private lastTriangles = 0;
  private lastPoints = 0;
  private maxTriangles = 0;
  private lastFlushMs: number | null = null;

  constructor(private readonly emit: (event: LiveMeshStatsEvent) => void, private readonly intervalMs = LIVE_MESH_STATS_INTERVAL_MS) {}

  recordBuild(elapsedMs: number, triangles: number, points: number): void {
    this.builds += 1; this.buildMs.push(elapsedMs); this.lastTriangles = triangles; this.lastPoints = points; this.maxTriangles = Math.max(this.maxTriangles, triangles);
  }
  recordUpload(elapsedMs: number): void { this.uploadMs.push(elapsedMs); }
  recordFrame(renderElapsedMs: number): void { this.frames += 1; this.renderMs.push(renderElapsedMs); }

  /** Cheap enough to call every frame: it only compares two numbers until the interval has passed. */
  maybeFlush(nowMs: number, context: { mode: string }): void {
    if (this.lastFlushMs === null) { this.lastFlushMs = nowMs; return; }
    if (nowMs - this.lastFlushMs < this.intervalMs) return;
    this.flush(nowMs, context);
  }

  flush(nowMs: number, context: { mode: string }): void {
    const intervalMs = this.lastFlushMs === null ? 0 : Math.round(nowMs - this.lastFlushMs);
    this.lastFlushMs = nowMs;
    if (this.builds > 0) {
      const b = this.buildMs.stats();
      this.emit({ event: 'XR_LIVE_MESH_BUILD_STATS', fields: { mode: context.mode, intervalMs, builds: this.builds, buildMsP50: b.p50, buildMsP95: b.p95, buildMsMax: b.max, triangles: this.lastTriangles, maxTriangles: this.maxTriangles, points: this.lastPoints } });
    }
    if (this.frames > 0) {
      const r = this.renderMs.stats(); const u = this.uploadMs.stats();
      this.emit({ event: 'XR_LIVE_MESH_RENDER_STATS', fields: { mode: context.mode, intervalMs, frames: this.frames, renderMsP50: r.p50, renderMsP95: r.p95, renderMsMax: r.max, uploads: this.uploadMs.size, uploadMsP95: u.p95, triangles: this.lastTriangles } });
    }
    this.builds = 0; this.frames = 0; this.maxTriangles = 0;
    this.buildMs.clear(); this.renderMs.clear(); this.uploadMs.clear();
  }
}
