import type { WorkflowStep } from '../../domain/device';

const FAST_STEPS = new Set(['CALIBRATING', 'PLACEMENT', 'RANGING', 'RECORDING', 'CALCULATING']);

export function workflowStatusPollDelay(step: WorkflowStep | string | undefined, succeeded: boolean, failures: number): number {
  if (!succeeded) return Math.min(30000, 5000 * (2 ** Math.min(Math.max(failures, 0), 4)));
  return step !== undefined && FAST_STEPS.has(step) ? 1000 : 5000;
}
