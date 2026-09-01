export interface StructuralGuideWorkerResultIdentity {
  requestId: number;
  epoch: number;
}

export interface StructuralGuidePendingIdentity {
  id: number;
  epoch: number;
}

/** Pure protocol guard so stale worker replies can be unit-tested without loading a Worker bundle. */
export function isCurrentStructuralGuideWorkerResult(
  message: StructuralGuideWorkerResultIdentity,
  pending: StructuralGuidePendingIdentity | null,
  currentEpoch: number,
): boolean {
  return pending !== null &&
    message.requestId === pending.id &&
    message.epoch === pending.epoch &&
    message.epoch === currentEpoch;
}
