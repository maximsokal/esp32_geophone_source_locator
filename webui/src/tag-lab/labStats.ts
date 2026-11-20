/** Bounded sample window with percentile queries (development tooling only). */
export class RollingSamples {
  private readonly values: number[] = [];
  constructor(private readonly capacity = 240) {}
  push(value: number): void {
    if (!Number.isFinite(value)) return;
    this.values.push(value);
    if (this.values.length > this.capacity) this.values.shift();
  }
  get count(): number { return this.values.length; }
  percentile(p: number): number | null {
    if (this.values.length === 0) return null;
    const sorted = [...this.values].sort((a, b) => a - b);
    const rank = Math.min(sorted.length - 1, Math.max(0, Math.ceil((p / 100) * sorted.length) - 1));
    return sorted[rank];
  }
  clear(): void { this.values.length = 0; }
}

/** Events per second over a sliding window of timestamps (ms). */
export class RateCounter {
  private readonly stamps: number[] = [];
  constructor(private readonly windowMs = 2000) {}
  mark(nowMs: number): void {
    this.stamps.push(nowMs);
    this.trim(nowMs);
  }
  perSecond(nowMs: number): number {
    this.trim(nowMs);
    if (this.stamps.length < 2) return 0;
    const span = nowMs - this.stamps[0];
    return span <= 0 ? 0 : (this.stamps.length / span) * 1000;
  }
  clear(): void { this.stamps.length = 0; }
  private trim(nowMs: number): void {
    while (this.stamps.length > 0 && nowMs - this.stamps[0] > this.windowMs) this.stamps.shift();
  }
}
