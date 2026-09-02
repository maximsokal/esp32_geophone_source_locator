import { deviceComputeProfilePromise } from './deviceComputeProfile';

export type LiveComputeTaskKind = 'structural' | 'visual';

export interface LiveComputeLease {
  readonly kind: LiveComputeTaskKind;
  release(): void;
}

export interface LiveComputeSchedulerSnapshot {
  budget: number | null;
  active: number;
  structural: number;
  visual: number;
}

type BudgetProvider = () => Promise<number>;

function safeBudget(value: number): number {
  return Number.isFinite(value) ? Math.max(0, Math.floor(value)) : 0;
}

/**
 * Global concurrency gate for CPU-heavy work while immersive XR is live.
 *
 * Worker creation is cheap compared with the actual Hough/structural pass, so
 * the scheduler limits simultaneously ACTIVE jobs rather than idle Worker
 * objects. Acquisition never queues: stale visual/structural work is less useful
 * than a later fresh keyframe, therefore callers simply retry on their normal
 * cadence when all live slots are occupied.
 */
export class LiveComputeScheduler {
  private active = 0;
  private activeStructural = 0;
  private activeVisual = 0;
  private resolvedBudget: number | null = null;

  constructor(private readonly budgetProvider: BudgetProvider) {}

  async tryAcquire(kind: LiveComputeTaskKind): Promise<LiveComputeLease | null> {
    let budget = 0;
    try {
      budget = safeBudget(await this.budgetProvider());
    } catch {
      // Capability discovery is advisory. If it ever fails unexpectedly, do not
      // leave clients stuck in startup and do not guess that parallelism is safe.
      budget = 0;
    }
    this.resolvedBudget = budget;
    // Promise continuations execute serially on the JS event loop, so this
    // check+increment is atomic with respect to competing main-thread callers.
    if (budget <= 0 || this.active >= budget) return null;

    this.active += 1;
    if (kind === 'structural') this.activeStructural += 1;
    else this.activeVisual += 1;

    let released = false;
    return {
      kind,
      release: () => {
        if (released) return;
        released = true;
        this.active = Math.max(0, this.active - 1);
        if (kind === 'structural') this.activeStructural = Math.max(0, this.activeStructural - 1);
        else this.activeVisual = Math.max(0, this.activeVisual - 1);
      },
    };
  }

  snapshot(): LiveComputeSchedulerSnapshot {
    return {
      budget: this.resolvedBudget,
      active: this.active,
      structural: this.activeStructural,
      visual: this.activeVisual,
    };
  }
}

export const liveComputeScheduler = new LiveComputeScheduler(async () =>
  (await deviceComputeProfilePromise).liveWorkerBudget
);
