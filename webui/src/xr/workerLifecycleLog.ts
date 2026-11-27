import { clientDiagnostics } from '../data/clientDiagnostics';

/**
 * One bounded SCAN_WORKER_LIFECYCLE record whenever a long-lived scan worker is created or ended (TASK 06/11): which worker, why, and how many of its kind are
 * alive after the change, so a worker that is never released (or one that is called after its end) shows in the log instead of only in a thrown error.
 * Workers are created a handful of times per scan, never per frame, so no rate limit is needed.
 */
const alive = new Map<string, number>();

export function logWorkerLifecycle(worker: string, action: 'create' | 'terminate', reason: string): void {
  const count = Math.max(0, (alive.get(worker) ?? 0) + (action === 'create' ? 1 : -1));
  alive.set(worker, count);
  clientDiagnostics.info('SCAN_WORKER_LIFECYCLE', { worker, action, reason: reason.slice(0, 80), alive: count });
}

/** Test seam: forgets the counts between tests. */
export function resetWorkerLifecycleCounts(): void { alive.clear(); }
